# FLOMPY

** A floppy disk dumping program for DOS. **

Brad Smith, 2019

[Patreon](https://www.patreon.com/rainwarrior)

[Website](http://rainwarrior.ca/)

## Purpose

I have a floppy drive in a machine that's not new enough to use
[fdrawcmd.sys](https://simonowen.com/fdrawcmd/)
so I wanted to find a dumping program for DOS.

There are a few such programs that already exist,
but I also wanted to experiment with directly operating the
floppy disk controller, in case it could help recovering disks with
corrupt or strange formatting. I couldn't find anything open-source,
so I decided to write FLOMPY.

Ultimately I think it's more useful as an open-source example than
anything else, as the FDC capabilities for extracting raw track data seem
to be a bit limited.

## Usage

```
 -m boot          Display boot sector information, no file output.
 -m high <file>   Read disk image using BIOS. (Basic sector contents only.)
 -m low <file>    Read entire tracks.
 -m full <file>   Read all tracks, per-byte timing, and discover fuzzy bits.
 -m sector -t 5 -h 0 -s 3 <file>   Read a single sector using BIOS.
 -m track -t 5 -h 0 <file>         Read a single track.
 -m ftrack -t 5 -h 0 <file>        Read a single track, timing, fuzzy bits.
Options, automatic/default if unspecified:
 -b 512    Specify bytes per sector, default 512.
 -h 1      Specify total sides (1,2) default 2, or side (0,1).
 -t 80     Specify total tracks (cylinders) or track.
 -s 9      Specify sectors per track, or specific sector.
 -d 0      Specify device (0,1) = (A:,B:), default 0.
 -f 0xFF   Use a specific value to fill unreadable space, default 0.
Low level options:
 -r 1      Data rate (0,1,2,3) = (500 HD, 350, 250 DD, 1000 ED) k/s, default 1.
 -p 0      Port (0,1) = ($3FX,$37X), default 0.
 -e 1      Encoding (0,1) = (FM,MFM), default 1.
 -o 13 -l 15 -u 1   Timings o: stepper l: head load u: head unload.
```

### Notes:

* The `-m` option must always be used to specify a mode.
* Sectors begin counting from 1. Tracks and sides begin counting from 0.
* Low level reading requires an `-r` data rate option appropriate for the disk.
* The low level modes will only work properly in a pure DOS environment, not under Windows.

### Examples:

`FLOMPY -m boot`

Displays some boot sector information from the floppy.

`FLOMPY -m high <filename>`

High level sector-by-sector image dump.

`FLOMPY -m sector -t 5 -h 0 -s 3 <filename>`

High level single sector dump.

`FLOMPY -m low -r 0 <filename>`

Low level track read dump. See format notes below.

`FLOMPY -m full -r 0 <filename>`

Low level track read dump with per-byte timing. See format notes below.

`FLOMPY -m track -r 0 -t 5 -h 0 <filename>`

Low level single track dump.

`FLOMPY -m ftrack -r 0 -t 5 -h 0 <filename>`

Low level single track dump with per-byte timing.

## Sector Dump Format

The `high` dumps use the BIOS (`int 13h`) to read sectors from the disk.
The image format produced is a standard floppy disk dump,
where all sectors are the same size, and the file just stores all of them
in C:H:S order, meaning **cylinder** (track), **head** (side), **sector**.
The first sector in the file is 0:0:1, then 0:0:2... after all the
sectors on one side are read, we continue on the other side at 0:1:1, 0:1:2...
and finally proceed to the next track 1:0:1, 1:0:2...

Any sectors that could not be read will instead be filled with the fill value
specified with the `-f` option. Disks with more unusual formatting can't really
be represented in this kind of file.

## Track Dump Format

For the low level track dumps (`low`/`full`), each track begins with two bytes
indicating the track and side. Following this is a 4-byte integer (little-endian)
specifying how many bytes were read from the track. After this, the indicated
number of bytes of data appear.

If using the `full`/`ftrack` timed read option, immediately following the track
data bytes are a string of 2-byte integers indicating the timing for each byte
that was read. This timer begins at 0 with the first byte, and indicates how many
ticks of the 1,193,182 Hz PIT timer had passed since that first byte.

A single track dump (`track`/`ftrack`) omits the two byte track/side prefix.

The raw track data is read using the "read track" command of the floppy disk
controller. This seems to search for the start of the first sector on the track,
and begins reading from there. This will continue reading for about 16000 bytes,
including gaps and other raw data that appears between sectors.

I could not find clear documentation of the function and
parameters to the read track command. I was really hoping it would be able to
read a single revolution of the disk from index to index, but instead it seems
to require starting from inside a sector, making it tricky to determine what
data may have appeared before the start of that first sector. I set the starting
sector as "0", though this seems to behave the same as "1", and told it the
expected sector size was 16k so that it would deliver a long uninterrupted
read. (Please contact me if you have any useful experience here that could help.)

Mostly it seems that this could be used for trying to extract information from
sectors that have gone bad. If they're failing to read in the high level
version, this might give an opportunity to recover some of the damaged data.

However, even when the data is undamaged, the information read back using this
method seems to be slightly inconsistent, and it may be worth taking multiple
readings.
