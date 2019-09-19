//
// FLOMPY
// A floppy disk dumper for DOS environments.
//
// Version 0
// Brad Smith, 2019
// http://rainwarrior.ca
// https://github.com/bbbradsmith/flompy
//
// Compiled with Open Watcom, 16-Bit real mode, large memory model
// http://openwatcom.org
//

#include <bios.h>     // _bios_disk
#include <conio.h>    // inp, outp
#include <dos.h>      // _dos_getvect, _dos_setvect
#include <i86.h>      // _interrupt, _disable, _enable
#include <limits.h>   // INT_MIN, INT_MAX
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // getopt

const int VERSION = 0;

// maximum sector size for high level read buffer
#define MAX_SECTOR_SIZE   2048

// number of retries for BIOS operations
#define HIGH_RETRIES   8

// maximum track size for low level read buffer
// (chosen so that MAX_TRACK_SIZE * 2 < 64k so timing can fit in a segment)
// keep this equal to MAX_TRACK_SIZE in flompirq.asm
#define MAX_TRACK_SIZE   31000

// timeout for low level IRQ in system clock ticks (~18 times per second)
#define LOW_TIMEOUT   (10*18)

// number of retries for low level seek and read operations
#define SEEK_RETRIES   8
#define READ_RETRIES   4

// Exit codes, later versions may append to but not reorder this list
enum {
        RESULT_SUCCESS  = 0, // success
        RESULT_ARGS     = 1, // argument failure
        RESULT_RESET    = 2, // failure to read boot sector (-m boot)
        RESULT_BOOT     = 3, //
        RESULT_OUTPUT   = 4, // unable to open output file
        RESULT_MODE     = 5, // unexpected mode
        RESULT_TODO     = 6, // unimplemented feature
        RESULT_PARTIAL  = 7, // partial success, output produced but with errors
        RESULT_FATAL    = 8, // fatal error, no output produced
        RESULT_MEMORY   = 9, // out of memory
        RESULT_LOW      = 10, // unable to begin low level control
};

typedef uint32_t     uint32;
typedef uint16_t     uint16;
typedef uint8_t      uint8;
typedef unsigned int uint;

// command line parameters
int sector_bytes = -1;
int track_sectors = -1;
int tracks = -1;
int sides = -1;
int device = 0;
int datarate = 1;
int fill = 0x00;
int mode = -1;
int fdc_port = 0;
int encoding = 1;
int rate_step = 13; // default suggested as "typical" by fdrawcmd
int rate_load = 15; // ''
int rate_unload = 1; // ''
const char* filename = NULL;

// parameters auto-detected from boot sector
int boot_sector_bytes = -1;
int boot_track_sectors = -1;
int boot_total_sectors = -1;
int boot_sides = -1;

FILE* f = NULL; // output file

// high level read buffer
uint8 highdata[MAX_SECTOR_SIZE];

// low level read buffers
volatile uint lowpos; // bytes read from track
uint8* lowdata = NULL; // complete track
uint16* lowtime = NULL; // timing values
int lowtime_on = 0;
int lowport;

uint8 pic0_mask_old;
void (__interrupt __far *floppy_irq_old)() = NULL;
volatile int floppy_irq_wait;
uint8 floppy_st0;
uint8 floppy_st1;
uint8 floppy_st2;
uint8 floppy_c;
uint8 floppy_h;
uint8 floppy_r;
uint8 floppy_n;

//
// misc functions
//

void open_output(); // exit(RESULT_OUTPUT) if file could not be opened

void* get_memory(size_t size) // exit(RESULT_MEMORY) if could not be allocated
{
        void* p = malloc(size);
        if (p == NULL)
        {
                fprintf(stderr,"Out of memory.\n");
                exit(RESULT_MEMORY);
        }
        return p;
}

void free_all()
{
        if (f != NULL) fclose(f);
        free(lowdata); lowdata = NULL;
        free(lowtime); lowtime = NULL;
}

void printparam(int p) // for unspecified parameter diagnostic
{
        if (p < 0) printf("UNKNOWN");
        else printf("%d",p);
}

void dump(const uint8* buffer, int length) // dump hex 
{
        int i;
        for (i=0; i<length; ++i)
        {
                if ((i & 31) == 0) printf("%04X: ",i);
                printf("%02X",buffer[i]);
                if ((i & 31) == 31) printf("\n");
                else if ((i & 7) == 7) printf(" ");
        }
        if ((length & 31) != 0) printf("\n");
}

//
// high level BIOS operations
//

const char* const UNKNOWN_HIGH_ERROR = "Unknown INT 13h error";
struct diskinfo_t diskinfo;

typedef struct { uint8 code; const char* const text; } BiosErrorCode;
const BiosErrorCode HIGH_ERROR[] = {
        { 0x00, "Success" },
        { 0x01, "Bad command" },
        { 0x02, "Address mark not found" },
        { 0x03, "Attempt to write to write-protected disk" },
        { 0x04, "Sector not found" },
        { 0x05, "Reset failed" },
        { 0x06, "Disk changed since last operation" },
        { 0x07, "Drive parameter activity failed" },
        { 0x08, "DMA overrun" },
        { 0x09, "Attempt to DMA across 64kb boundary" },
        { 0x0A, "Bad sector detected" },
        { 0x0B, "Bad track detected" },
        { 0x0C, "Media type not found" },
        { 0x0D, "Invalid number of sector" },
        { 0x0E, "Control data address mark detected" },
        { 0x0F, "DMA out of range" },
        { 0x10, "Data read CRC/ECC error" },
        { 0x11, "CRC/ECC corrected data error" },
        { 0x20, "Controller failure" },
        { 0x40, "Seek operation failed" },
        { 0x80, "Disk timed out or failed to respond" },
        { 0xAA, "Drive not ready" },
        { 0xBB, "Undefined error" },
        { 0xCC, "Write fault" },
        { 0xE0, "Status error" },
        { 0xFF, "Sense operation failed" },
};

const char* high_error(uint8 e)
{
        int i;
        for (i=0; i < (sizeof(HIGH_ERROR)/sizeof(HIGH_ERROR[0])); ++i)
        {
                if (HIGH_ERROR[i].code == e) return HIGH_ERROR[i].text;
        }
        return UNKNOWN_HIGH_ERROR;
};

uint8 high_retry(unsigned service) // retries a BIOS operation multiple times or until success
{
        uint8 result;
        int i = HIGH_RETRIES;
        for (; i; --i)
        {
                result = _bios_disk(service, &diskinfo) >> 8;
                if (result == 0) break;
        }
        return result;
}

uint8 high_reset()
{
        diskinfo.drive = device;
        diskinfo.head = 0;
        diskinfo.track = 0;
        diskinfo.sector = 0;
        diskinfo.nsectors = 0;
        diskinfo.buffer = highdata;
        return high_retry(_DISK_RESET);
}

uint8 high_read_sector(int track, int side, int sector)
{
        memset(highdata, fill & 0xFF, sizeof(highdata));
        diskinfo.drive = device;
        diskinfo.head = side;
        diskinfo.track = track;
        diskinfo.sector = sector;
        diskinfo.nsectors = 1;
        diskinfo.buffer = highdata;
        return high_retry(_DISK_READ);
}

uint16 high16(int pos) // fetch 16-bit little-endian value from highdata
{
        return (highdata[pos+0] << 0)
             | (highdata[pos+1] << 8);
}

//
// low level operations
//

const char* const UNKNOWN_LOW_ERROR = "Unknown FDC error";

enum {
        LOW_SUCCESS  = 0,
        LOW_RESET,
        LOW_CALIBRATE_TIMEOUT,
        LOW_CALIBRATE,
        LOW_SEEK_TIMEOUT,
        LOW_SEEK,
        LOW_TRACK_TIMEOUT,
                LOW_EMPTY,
        LOW_COUNT
};

const char* const LOW_ERROR[LOW_COUNT] = {
        "Success",
        "Reset IRQ timeout",
        "Calibrate IRQ timeout",
        "Calibration failure",
        "Seek IRQ timeout",
        "Seek failure",
        "Read track IRQ timeout",
                "No data read from track",
};

const char* low_error(uint8 e)
{
        if (e >= LOW_COUNT) return UNKNOWN_LOW_ERROR;
        return LOW_ERROR[e];
}

extern void __interrupt _far floppy_irq();
extern volatile uint lowpos;
extern uint8* lowdata;
extern uint16* lowtime;
extern int lowtime_on;
extern int lowport;
extern volatile int floppy_irq_wait;

// This was replaced with assembly, but left here for reference.
// To see the code this generates, build the .obj and use Watcom's disassembler
// to create FLOMPY.LST:
//     C:\WATCOM\OWSETENV.BAT
//     WDIS -l flompy
void __interrupt __far floppy_irq_unused()
{
	if (inp(lowport|4) & 0x20) // non-DMA data flag
	{
		uint8 data = inp(lowport|5);
		if (lowpos < MAX_TRACK_SIZE)
		{
			lowdata[lowpos] = data;
			if (lowtime_on)
			{
				// read system timer (16 bit counter that decrements at 1,193,182 Hz)
				outp(0x43,0x00);
				lowtime[lowpos]  = inp(0x40);
				lowtime[lowpos] |= inp(0x40) << 8;
			}
			++lowpos;
		}
	}
	else // is a result IRQ
	{
		//printf("IRQ status: %02X\n",status);
		floppy_irq_wait = 0;
	}
	outp(0x20,0x20); // end of interrupt
}

void floppy_irq_install()
{
	_disable();
	floppy_irq_old = _dos_getvect(0x0E);
	_dos_setvect(0x0E, floppy_irq);
	pic0_mask_old = inp(0x21);
	outp(0x21, pic0_mask_old & (~(1<<6))); // unmask floppy IRQ (6)
	_enable();
}

void floppy_irq_restore()
{
	_disable();
	_dos_setvect(0x0E, floppy_irq_old);
	outp(0x21, pic0_mask_old);
	floppy_irq_old = NULL;
	_enable();
}

void delay(uint ticks) // delay in ~1/18 second ticks
{
	long time0;
	long time1;
	_bios_timeofday(_TIME_GETCLOCK,&time0);
	for (; ticks>0; --ticks)
	{
		do
		{
			_bios_timeofday(_TIME_GETCLOCK,&time1);
		} while(time0 == time1);
		time0 = time1;
	}
}

int floppy_write(uint8 value)
{
	uint16 timeout = 0;
	do
	{
		if (inp(lowport|4) & 0x80)
		{
			outp(lowport|5, value);
			return 0;
		}
		++timeout;
	} while (timeout);
	//printf("write timeout\n"); // debug
	return -1;
}

uint8 floppy_read()
{
	uint16 timeout = 0;
	do
	{
		if (inp(lowport|4) & 0x80)
		{
			return inp(lowport|5);
		}
		++timeout;
	} while (timeout);
	//printf("read timeout\n"); // debug
	return 0xFF;
}

int floppy_irq_status()
{
	floppy_st0 = 0xFF;
	floppy_c   = 0;
	if (floppy_write(0x08)) return -1;
	floppy_st0 = floppy_read();
	floppy_c   = floppy_read();
	return 0;
}

int floppy_irq_wait_timeout()
{
	uint16 short_timeout;
	long timestart = 0;
	long timenow;
	do
	{
		// check wait flag 65536 times 
		short_timeout = 0;
		do
		{
			if (!floppy_irq_wait) return 0; // success
			++short_timeout;
		} while (short_timeout);
		// check the system clock for timeout
		if (timestart == 0) _bios_timeofday(_TIME_GETCLOCK,&timestart);
		else
		{
			_bios_timeofday(_TIME_GETCLOCK,&timenow);
			timenow -= timestart;
			if (timenow >= LOW_TIMEOUT) break;
		}
	} while (1);
	return -1; // timeout
}

void low_close()
{
	outp(lowport|2, 0x00 | device); // put in reset state, motor off
	floppy_irq_restore();
}

uint8 low_open()
{
	int i;

	floppy_irq_install();

	outp(lowport|2, 0x00 | device); // begin reset
	delay(10); // wait 500 ms
	floppy_irq_wait = 1;
	outp(lowport|2, 0x0C | device); // end reset
	if (floppy_irq_wait_timeout())
	{
		low_close();
		return LOW_RESET;
	}

	// read 4 times to clear
	floppy_irq_status();
	floppy_irq_status();
	floppy_irq_status();
	floppy_irq_status();

	outp(lowport|4, datarate); // set speed

	// set timing parameters and no-DMA mode
	floppy_write(0x03);
	floppy_write((rate_step << 4) | rate_load);
	floppy_write((rate_unload << 1) | 1); // no-DMA mode

	// turn on the motor
	outp(lowport|2, (0x10 << device) | 0x0C | device);
	delay(3);

	// calibrate
	for (i=0; i < SEEK_RETRIES; ++i)
	{
		floppy_irq_wait = 1;
		floppy_write(0x07);
		floppy_write(device);
		if (floppy_irq_wait_timeout())
		{
			low_close();
			return LOW_CALIBRATE_TIMEOUT;
		}
		floppy_irq_status();
		if (floppy_c == 0) break;
	}
	if (floppy_c != 0)
	{
		low_close();
		return LOW_CALIBRATE;
	}

	// motor is on, IRQ is installed

	return LOW_SUCCESS;
}

uint8 low_read_track(int track, int side)
{
	int i;

	// seek to track
	for (i=0; i < SEEK_RETRIES; ++i)
	{
		floppy_irq_wait = 1;
		floppy_write(0x0F);
		floppy_write((side << 2) | device);
		floppy_write(track);
		if (floppy_irq_wait_timeout())
		{
			return LOW_SEEK_TIMEOUT;
		}
		floppy_irq_status();
		if (floppy_c == track) break;
	}
	if (floppy_c != track)
	{
		return LOW_SEEK;
	}
	delay(3); // let the head settle

	// read track
	lowpos = 0;
	for (i=0; lowpos==0 && i<READ_RETRIES; ++i)
	{
		floppy_irq_wait = 1;
		floppy_write((encoding << 6) | 0x02);
		floppy_write((side << 2) | device);
		floppy_write(track);
		floppy_write(side);
		floppy_write(0); // starting sector?
	    floppy_write(0x07); // sector bytes, 07 = 16k (largest value within spec)
		floppy_write(0xFF); // keep reading until sector 255 or index
		floppy_write(0); // gap length (ignored?)
		floppy_write(0xFF); // data length
		if (floppy_irq_wait_timeout())
		{
			return LOW_TRACK_TIMEOUT;
		}
		// consume results
		floppy_st0 = floppy_read();
		floppy_st1 = floppy_read();
		floppy_st2 = floppy_read();
		floppy_c   = floppy_read();
		floppy_h   = floppy_read();
		floppy_r   = floppy_read();
		floppy_n   = floppy_read();
		//floppy_irq_status();
	}

	if (lowpos < 1) return LOW_EMPTY;
	return LOW_SUCCESS;
}

//
// high level modes
//

int mode_boot()
{
	int i;
	if (highdata[0x26] == 0x29)
	{
		printf("$027 ID: ");
		for (i=0; i<4; ++i) printf("%02X ",highdata[0x27+i]);
		printf("\n");
		printf("$02B Label: [");
		for (i=0; i<11; ++i) printf("%c",highdata[0x2B+i]);
		printf("]\n");
	}
	printf("$00B Bytes per sector:   %d\n", boot_sector_bytes);
	printf("$013 Total sectors:      %d\n", high16(0x013));
	printf("$018 Sectors per track:  %d\n", boot_track_sectors);
	printf("$01C Sides:              %d\n", boot_sides);
	if (boot_total_sectors == 0)
	{
		printf("$020 Long total sectors: $");
		for (i=0; i<4; ++i) printf("%02X",highdata[0x23-i]);
		printf("\n");
	}
	printf("Completed.\n");
	return RESULT_SUCCESS;
}

int mode_high()
{
	int c,h,s;
	int invalid;
	uint8 result;

	// auto detection
	if (sector_bytes < 0) sector_bytes = boot_sector_bytes;
	if (sector_bytes < 0) sector_bytes = 512; // default
	if (track_sectors < 0) track_sectors = boot_track_sectors;
	if (sides < 0) sides = boot_sides;
	if (tracks < 0)
	{
		// if not yet specified, assume number of sides based on number of sectors
		if (sides <= 0)
		{
			sides = (boot_total_sectors > 0 && boot_total_sectors < 1000) ? 1 : 2;
		}
		// automatic track count, rounding up to include all sepcified sectors
		tracks = (boot_total_sectors + ((track_sectors * sides) - 1)) / (track_sectors * sides);
	}
	if (sides <= 0 || sides > 2) sides = 2; // default to 2

	// actual parameters
	printf("High: ");
	printparam(tracks);
	printf(" tracks, ");
	printparam(sides);
	printf(" sides, ");
	printparam(track_sectors);
	printf(" sectors, ");
	printparam(sector_bytes);
	printf(" bytes\n");

	invalid = 0;
	if (tracks < 0) { fprintf(stderr,"Track count unspecified.\n"); invalid=1; }
	if (track_sectors < 0) { fprintf(stderr,"Sectors per track unspecified.\n"); invalid=1; }
	if (sector_bytes > MAX_SECTOR_SIZE) { fprintf(stderr,"Sector size too large. Maximum: %d\n",MAX_SECTOR_SIZE); invalid=1; }
	if (invalid) return RESULT_FATAL; // fatal error

	open_output();

	invalid = 0;
	for (c=0; c<tracks; ++c)
	for (h=0; h<sides; ++h)
	for (s=1; s<=track_sectors; ++s)
	{
		printf("%02d:%02d:%02d\r",c,h,s);
		fflush(stdout); // because \r is not a newline it doesn't flush automatically
		result = high_read_sector(c,h,s);
		if (result)
		{
			++invalid;
			fprintf(stderr,"%02d:%02d:%02d error: %s\n",c,h,s,high_error(result));
		}
		fwrite(highdata,1,sector_bytes,f);
	}
	
	if (invalid)
	{
		printf("Completed, with errors.\n");
		return RESULT_PARTIAL;
	}
	printf("Completed.\n");
	return RESULT_SUCCESS;
}

int mode_sector()
{
	int c,h,s;
	int invalid;
	uint8 result;

	// auto detection
	if (sector_bytes < 0) sector_bytes = boot_sector_bytes;
	if (sector_bytes < 0) sector_bytes = 512; // default

	// actual parameters
	printf("Sector: track ");
	printparam(tracks);
	printf(", side ");
	printparam(sides);
	printf(", sector ");
	printparam(track_sectors);
	printf(", ");
	printparam(sector_bytes);
	printf(" bytes\n");

	invalid = 0;
	if (tracks < 0) { fprintf(stderr,"Track unspecified.\n"); invalid=1; }
	if (sides < 0) { fprintf(stderr,"Side unspecified.\n"); invalid=1; }
	if (track_sectors < 0) { fprintf(stderr,"Sector unspecified.\n"); invalid=1; }
	if (sector_bytes > MAX_SECTOR_SIZE) { fprintf(stderr,"Sector size too large. Maximum: %d\n",MAX_SECTOR_SIZE); invalid=1; }
	if (invalid) return RESULT_FATAL; // fatal error

	open_output();

	c = tracks;
	h = sides;
	s = track_sectors;
	printf("%02d:%02d:%02d\r",c,h,s);
	fflush(stdout);
	result = high_read_sector(c,h,s);
	if (result)
	{
		++invalid;
		fprintf(stderr,"%02d:%02d:%02d error: %s\n",c,h,s,high_error(result));
	}
	else printf("\n");
	fwrite(highdata,1,sector_bytes,f);
	
	if (invalid)
	{
		printf("Completed, with errors.\n");
		return RESULT_PARTIAL;
	}
	printf("Completed.\n");
	return RESULT_SUCCESS;
}

//
// low level modes
//

const char* DATARATE[4] = { "500", "350", "250", "1000" };

int mode_low_start(const char* name)
{
	int invalid;
	
	// auto detection
	if (sides < 0) sides = boot_sides;
	if (track_sectors < 0) track_sectors = boot_track_sectors;
	if (tracks < 0)
	{
		// if not yet specified, assume number of sides based on number of sectors
		if (sides <= 0)
		{
			sides = (boot_total_sectors > 0 && boot_total_sectors < 1000) ? 1 : 2;
		}
		// automatic track count, rounding up to include all sepcified sectors
		tracks = (boot_total_sectors + ((track_sectors * sides) - 1)) / (track_sectors * sides);
	}
	if (sides <= 0 || sides > 2) sides = 2; // default to 2

	// actual parameters
	printf("%s: ", name);
	printparam(tracks);
	printf(" tracks, ");
	printparam(sides);
	printf(" sides, %skb/s data rate, port $%03X, %s encoding\n",
		DATARATE[datarate],
		lowport,
		encoding ? "MFM" : "FM");
	printf("Timing: %d stepper motor, %d head load, %d head unload\n",
		rate_step, rate_load, rate_unload );

	invalid = 0;
	if (tracks < 0) { fprintf(stderr,"Track count unspecified.\n"); invalid=1; }
	if (invalid) return RESULT_FATAL; // fatal error	
	return RESULT_SUCCESS;
}

void mode_low_track_write()
{
	uint i;
	uint16 t;
	uint32 w = lowpos; // convert to 32 bit value
	fwrite(&w,4,1,f);
	fwrite(lowdata,1,lowpos,f);
	if (lowtime_on)
	{
		// convert count-down timer to count-up relative to first time
		t = 0xFFFF - lowtime[0];
		for  (i=0; i<lowpos; ++i)
		{
			lowtime[i] = (0xFFFF - lowtime[i]) - t;
		}
		fwrite(lowtime,2,lowpos,f);
	}
}

int mode_low_finish()
{
	int c,h;
	int invalid;
	uint8 result;
	uint32 bytes_read = 0;

	open_output();

	invalid = 0;
	for (c=0; c<tracks; ++c)
	for (h=0; h<sides; ++h)
	{
		printf("%02d:%02d\r",c,h);
		fflush(stdout);
		result = low_open();
		if (!result) result = low_read_track(c,h);
		if (result)
		{
			++invalid;
			fprintf(stderr,"%02d:%02d error: %s\n",c,h,low_error(result));
		}
		low_close();
		// not certain why I need to close/open for each track read,
		// but it might get interrupted by the file write?
		mode_low_track_write();
		bytes_read += lowpos;
	}

	if (invalid)
	{
		printf("Completed, with errors.\n");
			return RESULT_PARTIAL;
	}
	printf("Completed (%ld bytes read).\n", bytes_read);
	return RESULT_SUCCESS;
}

int mode_low()
{
	uint8 result;
	result = mode_low_start("Low");
	if (result != RESULT_SUCCESS) return result;

	// allocate memory and open output
	lowdata = get_memory(MAX_TRACK_SIZE);
	lowtime_on = 0;

	return mode_low_finish();
}

int mode_full()
{
	uint8 result;
	result = mode_low_start("Low");
	if (result != RESULT_SUCCESS) return result;

	// allocate memory and open output
	lowtime = get_memory(MAX_TRACK_SIZE*2);
	lowdata = get_memory(MAX_TRACK_SIZE);
	lowtime_on = 1;

	return mode_low_finish();
}

int mode_track_start(const char* name)
{
	int invalid;
	
	// actual parameters
	printf("%s: track ",name);
	printparam(tracks);
	printf(", side ");
	printparam(sides);
	printf(", %skb/s data rate, port $%03X, %s encoding\n",
		DATARATE[datarate],
		lowport,
		encoding ? "MFM" : "FM");
	printf("Timing: %d stepper motor, %d head load, %d head unload\n",
		rate_step, rate_load, rate_unload );

	invalid = 0;
	if (tracks < 0) { fprintf(stderr,"Track unspecified.\n"); invalid=1; }
	if (sides < 0) { fprintf(stderr,"Side unspecified.\n"); invalid=1; }
	if (invalid) return RESULT_FATAL; // fatal error
	return RESULT_SUCCESS;
}

int mode_track_finish()
{
	int c,h;
	int invalid;
	uint8 result;
	uint32 bytes_read = 0;

	open_output();

	result = low_open();
	if (result)
	{
		fprintf(stderr,"Error: %s\n", low_error(result));
		return RESULT_LOW;
	}

	invalid = 0;
	c = tracks;
	h = sides;
	printf("%02d:%02d\r",c,h);
	fflush(stdout);
	result = low_read_track(c,h);
	if (result)
	{
		++invalid;
		fprintf(stderr,"%02d:%02d error: %s\n",c,h,low_error(result));
	}
	else printf("\n");
	low_close();
	mode_low_track_write();
	bytes_read += lowpos;

	if (invalid)
	{
		printf("Completed, with errors.\n");
			return RESULT_PARTIAL;
	}
	printf("Completed (%ld bytes read).\n", bytes_read);
	return RESULT_SUCCESS;
}

int mode_track()
{
	uint8 result;
	result = mode_low_start("Track");
	if (result != RESULT_SUCCESS) return result;

	// allocate memory and open output
	lowdata = get_memory(MAX_TRACK_SIZE);
	lowtime_on = 0;

	return mode_track_finish();
}

int mode_ftrack()
{
	uint8 result;
	result = mode_low_start("Ftrack");
	if (result != RESULT_SUCCESS) return result;

	// allocate memory and open output
	lowtime = get_memory(MAX_TRACK_SIZE*2);
	lowdata = get_memory(MAX_TRACK_SIZE);
	lowtime_on = 1;

	return mode_track_finish();
}

//
// command line parsing and main program
//

enum {
	MODE_BOOT = 0,
	MODE_HIGH,
	MODE_LOW,
	MODE_FULL,
	MODE_SECTOR,
	MODE_TRACK,
	MODE_FTRACK,
	MODE_COUNT
};

const char* MODE_NAME[MODE_COUNT] = {
	"BOOT",
	"HIGH",
	"LOW",
	"FULL",
	"SECTOR",
	"TRACK",
	"FTRACK",
};

const char* ARGS_OPTS = ":b:h:t:s:d:f:r:p:e:o:l:u:m:";

const char* ARGS_INFO =
"Modes:\n"
" -m boot          Display boot sector information, no file output.\n"
" -m high <file>   Read disk image using BIOS. (Basic sector contents only.)\n"
" -m low <file>    Read entire tracks.\n"
" -m full <file>   Read all tracks, per-byte timing, and discover fuzzy bits.\n"
" -m sector -t 5 -h 0 -s 3 <file>   Read a single sector using BIOS.\n"
" -m track -t 5 -h 0 <file>         Read a single track.\n"
" -m ftrack -t 5 -h 0 <file>        Read a single track, timing, fuzzy bits.\n"
"Options, automatic/default if unspecified:\n"
" -b 512    Specify bytes per sector, default 512.\n"
" -h 1      Specify total sides (1,2) default 2, or side (0,1).\n"
" -t 80     Specify total tracks (cylinders) or track.\n"
" -s 9      Specify sectors per track, or specific sector.\n"
" -d 0      Specify device (0,1) = (A:,B:), default 0.\n"
" -f 0xFF   Use a specific value to fill unreadable space, default 0.\n"
"Low level options:\n"
" -r 1      Data rate (0,1,2,3) = (500 HD,350,250 DD,1000 ED) k/s, default 1.\n"
" -p 0      Port (0,1) = ($3FX,$37X), default 0.\n"
" -e 1      Encoding (0,1) = (FM,MFM), default 1.\n"
" -o 13 -l 15 -u 1   Timings o: stepper l: head load u: head unload.\n"
"FLOMPY version: %d\n"
;

void args_error()
{
	printf(ARGS_INFO,VERSION);
	exit(RESULT_ARGS);
}

void intarg(int* opt, int min, int max)
{
	char* n = "";
	errno = 0;
	*opt = strtol(optarg,&n,0);
	if (errno || *n != 0)
	{
		fprintf(stderr,"Could not parse integer argument.\n");
		args_error();
	}
	if (*opt < min || *opt > max)
	{
		fprintf(stderr,"Parameter %d out of range %d to %d.\n",*opt,min,max);
		args_error();
	}
}

void open_output() // exits if file could not be opened
{
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(stderr,"Unable to open output file: %s\n",filename);
		exit(RESULT_OUTPUT);
	}
	printf("Opened output file: %s\n",filename);
}

int main(int argc, char** argv)
{
	int i;
	int o;
	uint8 result;

	// parse the command line
	while (optind < argc)
	{
		do
		{
			o = getopt(argc,argv,":b:h:t:s:d:r:p:e:f:m:");
			if (o == -1) break;
			switch(o)
			{
				case 'b': intarg(&sector_bytes,128,MAX_SECTOR_SIZE); break;
				case 'h': intarg(&sides,0,2);                        break;
				case 't': intarg(&tracks,0,255);                     break;
				case 's': intarg(&track_sectors,0,255);              break;
				case 'd': intarg(&device,0,1);                       break;
				case 'f': intarg(&fill,INT_MIN,INT_MAX);             break;
				case 'r': intarg(&datarate,0,3);                     break;
				case 'p': intarg(&fdc_port,0,1);                     break;
				case 'e': intarg(&encoding,0,1);                     break;
				case 'o': intarg(&rate_step,0,15);                   break;
				case 'l': intarg(&rate_load,0,15);                   break;
				case 'u': intarg(&rate_unload,0,127);                break;
				case 'm':
					if (mode != -1)
					{
						fprintf(stderr,"Only one mode option allowed (-m).\n");
						args_error();
					}
					for (i=0;i<MODE_COUNT;++i)
					{
						if (!stricmp(MODE_NAME[i],optarg))
						{
							mode = i;
							break;
						}
					}
					if (mode < 0 || mode >= MODE_COUNT)
					{
						fprintf(stderr,"Invalid mode (-m).\n");
						args_error();
					}
					break;
				case '?':
					fprintf(stderr,"Unknown option -%c.\n",optopt);
					args_error();
					break;
				case ':':
					fprintf(stderr,"Missing parameter.\n");
					args_error();
					break;
				default:
					fprintf(stderr,"Unknown argument failure.\n");
					args_error();
					break;
			}
		} while (1);
		// getopt returned -1: possible filename
		if (optind < argc)
		{
			if (filename != NULL)
			{
				fprintf(stderr,"Only one output filename allowed.\n");
				args_error();
			}
			filename = argv[optind];
			++optind;
		}
	}

	if (mode < 0)
	{
		fprintf(stderr,"No mode selected. Use -m option.\n");
		args_error();
	}
	if (mode != MODE_BOOT && filename == NULL)
	{
		fprintf(stderr,"No output filename given.\n");
		args_error();
	}

	printf("Resetting BIOS disk system...");
	result = high_reset();
	if (result != 0)
	{
		printf("\n");
		fprintf(stderr,"BIOS disk reset failed.\n");
		return RESULT_RESET;
	}
	printf(" done.\n");

	printf("Reading boot sector for device %d...",device);
	result = high_read_sector(0,0,1);
	if (result != 0)
	{
		printf("\n");
		fprintf(stderr,"Boot sector not read, error %02Xh: %s\n", result, high_error(result));
		if (mode == MODE_BOOT) return RESULT_BOOT;
	}
	else
	{
		printf(" done.\n");
		boot_sector_bytes  = high16(0x00B);
		boot_total_sectors = high16(0x013);
		boot_track_sectors = high16(0x018);
		boot_sides         = high16(0x01A);
		if (boot_total_sectors == 0) boot_total_sectors = high16(0x020);
		//dump(highdata,128); // boot sector data debug
	}

	lowport = (fdc_port == 0) ? 0x3F0 : 0x370;

	switch(mode)
	{
	case MODE_BOOT:   result = mode_boot();   break;
	case MODE_HIGH:   result = mode_high();   break;
	case MODE_LOW:    result = mode_low();    break;
	case MODE_FULL:   result = mode_full();   break;
	case MODE_SECTOR: result = mode_sector(); break;
	case MODE_TRACK:  result = mode_track();  break;
	case MODE_FTRACK: result = mode_ftrack(); break;
	default:
		fprintf(stderr,"Unexpected mode (%d).\n",mode);
		result = RESULT_MODE;
	}
	
	free_all(); // also closes f
	return result;
}
