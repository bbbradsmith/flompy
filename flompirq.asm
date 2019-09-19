;
; FLOMPY
; Floppy Disk Controller IRQ handler
;

; keep this equal to MAX_TRACK_SIZE in flompy.c
MAX_TRACK_SIZE equ 31000

EXTRN _lowport         :word
EXTRN _lowtime_on      :word
EXTRN _lowpos          :word
EXTRN _floppy_irq_wait :word
EXTRN _lowdata         :dword
EXTRN _lowtime         :dword

PUBLIC floppy_irq_

.CODE
floppy_irq_:
    push ax
    push bx
    push dx
    push ds
    mov ax, DGROUP
    mov ds, ax
    mov dx, word ptr offset DGROUP:_lowport
    or dl, 4
    in al, dx
    test al, 0x20
    je result
; data read IRQ
    inc dx ; lowport|5
    in al, dx
    cmp word ptr offset DGROUP:_lowpos, MAX_TRACK_SIZE
    jae data_finish
    push es
    les bx,dword ptr offset DGROUP:_lowdata
    add bx, word ptr offset DGROUP:_lowpos
    mov byte ptr es:[bx], al
; optional timestamp
    cmp word ptr offset DGROUP:_lowtime_on, 0x0000
    je time_finish
    mov dx, 0x0043
    xor al, al
    out dx, al
    mov dx, 0x0040
    in al, dx
    mov bl, al
    in al, dx
    mov ah, al
    mov al, bl
    les bx,dword ptr offset DGROUP:_lowtime
    add bx, word ptr offset DGROUP:_lowpos
    add bx, word ptr offset DGROUP:_lowpos
    mov word ptr es:[bx], ax
time_finish:
    pop es
    inc word ptr offset DGROUP:_lowpos
data_finish:
    mov al, 0x20
    mov dx, 0x0020
    out dx, al
    pop ds
    pop dx
    pop bx
    pop ax
    iret
   
; command result
result:
    xor ax,ax
    mov word ptr offset DGROUP:_floppy_irq_wait, ax
    mov al, 0x20
    mov dx, 0x0020
    out dx, al
    pop ds
    pop dx
    pop bx
    pop ax
    iret

end

