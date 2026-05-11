; ESP8266 Wi-Fi Configuration ROM v.1.3
; PICOVERSE UART version
; Oduvaldo Pavan Junior
; ducasp@gmail.com
;    This ROM exposes only the ESP8266 setup menu when launched from
;    MSX PicoVerse Explorer F4. It does not install UNAPI hooks in memory.
;
; Pieces of this code were based on DENYOTCP.ASM (Denyonet ROM)
; made by Konamiman
;
; This code also has contributions from KdL, most specifically in the Wi-Fi
; setup menus and functionality, he has helped quite a lot to get it to the
; final format and functionality, thanks!
;
; The MSX PICO version of this code has contribution from Jeroen Taverne That
; helped making some MACROs on the fixed / non speed optimized loop I/O parts
; of the code so we can easily switch this from I/O based to Memory Based.
;
; Note: this implementation depends upon ESP8266 having compatible PicoVerse
; firmware flashed. This ROM only configures that firmware.
;
; Usage is free as long as you publish your code changes. If you make a profit
; selling something using this code, a suggestion is that you donate one or two
; pieces of the hardware to the MSX DEV contest to be awarded to MSX DEV winners
; as this shouldn't cost much and perhaps will bring interest on developers to
; make games that support internet :)
;
; If running the SETUP menu, we are going to "borrow" BASIC program area, that
; is saved at TXTTAB. We can't allocate memory at this point as any possible
; disk rom might be sitting on a slot that has not been initialized and results
; could be bad. The BASIC program area trick should be safe, unless some other
; cartridge runs a BASIC software for something (i.e.: wait disks be initialized
; ), in such case, that BASIC software or trick will most likely be corrupted
; after running our setup.
; ESC releases the ESP8266 hold, writes CONFIG_RETURN_MENU to the PicoVerse F2
; memory port, and restarts the MSX so Explorer can reload its menu.

;*************************
;***  BUILD DEFINITIONS **
;*************************

;--- Use memory mapped IO or not...
USE_MEM_IO:             equ 1

;*******************
;***  CONSTANTS  ***
;*******************

;--- System variables and routines:
CHGMOD:                 equ #005F
CHSNS:                  equ #009C
CHGET:                  equ #009F
CHPUT:                  equ #00A2
BEEP:                   equ #00C0
KILBUF:                 equ #0156
LINLEN:                 equ #F3B0
CLIKSW:                 equ #F3DB
TXTTAB:                 equ #F676
CSRSW:                  equ #FCA9


;--- Memory ports:
MEM_OUT_CMD_PORT:       equ #7F06
MEM_OUT_TX_PORT:        equ #7F07
MEM_IN_DATA_PORT:       equ #7F06
MEM_IN_STS_PORT:        equ #7F07
MEM_PORT_F2:            equ #7F05
CONFIG_RETURN_MENU:     equ #E0

;--- I/O ports:
OUT_CMD_PORT:           equ #06
OUT_TX_PORT:            equ #07
IN_DATA_PORT:           equ #06
IN_STS_PORT:            equ #07
PORT_F2:                equ #F2


    if USE_MEM_IO = 1
  MACRO SET_SPEED
    xor a
    ld  (MEM_OUT_CMD_PORT),a            ; Clear UART
  ENDM

    MACRO WRITE_CMD_PORT_A
        ld  (MEM_OUT_CMD_PORT),a
    ENDM

  MACRO CLEAR_UART
    ld  a,20
    ld  (MEM_OUT_CMD_PORT),a            ; Clear UART
  ENDM

  MACRO SEND_DATA
    ld  (MEM_OUT_TX_PORT),a             ; Just send, no need to wait response
  ENDM

  MACRO RECEIVE_DATA
    ld a,(MEM_IN_DATA_PORT)
  ENDM

  MACRO LOAD_STS_PORT_IN_A
    ld  a,(MEM_IN_STS_PORT)
  ENDM

  MACRO CHECK_DATA
    ld  a,(MEM_IN_STS_PORT)
    bit 0,a                         ; If nz has data
  ENDM

  MACRO CHECK_QUICK_RECEIVE
    ld  a,(MEM_IN_STS_PORT)
    bit 3,a                         ; Quick Receive Supported?
  ENDM

  MACRO CHECK_BUFFER_UNDERRUN
    ld  a,(MEM_IN_STS_PORT)
    bit 4,a                         ; Buffer underrun?
  ENDM

  MACRO READ_F2
    ld  a,(MEM_PORT_F2)
  ENDM

  MACRO WRITE_F2
    ld  (MEM_PORT_F2),a
  ENDM
    else

  MACRO SET_SPEED
    xor a
    out (OUT_CMD_PORT),a            ; Clear UART
  ENDM

    MACRO WRITE_CMD_PORT_A
        out (OUT_CMD_PORT),a
    ENDM

  MACRO CLEAR_UART
    ld  a,20
    out (OUT_CMD_PORT),a            ; Clear UART
  ENDM

  MACRO SEND_DATA
    out (OUT_TX_PORT),a             ; Just send, no need to wait response
  ENDM

  MACRO RECEIVE_DATA
    in a,(IN_DATA_PORT)
  ENDM

  MACRO LOAD_STS_PORT_IN_A
    in  a,(IN_STS_PORT)
  ENDM

  MACRO CHECK_DATA
    in  a,(IN_STS_PORT)
    bit 0,a                         ; If nz has data
  ENDM

  MACRO CHECK_QUICK_RECEIVE
    ld  a,(IN_STS_PORT)
    bit 3,a                         ; Quick Receive Supported?
  ENDM

  MACRO CHECK_BUFFER_UNDERRUN
    in  a,(IN_STS_PORT)
    bit 4,a                         ; Buffer underrun?
  ENDM

  MACRO READ_F2
    in  a,(PORT_F2)
  ENDM

  MACRO WRITE_F2
    out (PORT_F2),a
  ENDM
    endif

;--- Scan Page Size
SCAN_MAX_PAGE_SIZE      equ 8

;************************
;***  MSX ROM HEADER  ***
;************************
    org #4000
    db                  #41,#42
    dw                  INIT_CONFIG ; Enter Wi-Fi setup directly
    dw                  0           ; Statement
    dw                  0           ; Device
    dw                  0           ; Text
    ds                  6           ; Reserved

;==================
;===  Start-up  ===
;==================

INIT_CONFIG:
    ld  a,h                         ; Test if mirrored and executing in wrong page
    cp  #40
    ret nz
    xor a
    ld  (CSRSW),a                   ; cursor display is disabled
    ld  (CLIKSW),a                  ; key click disabled
    ld  a,1
    rst #30
    db  0
    dw  CHGMOD                      ; SCREEN 1
    ld  a,29                        ; WIDTH 29
    ld  (LINLEN),a
    jp  ENTERING_ESPSETUP

;============================
;===  SETUP Menu Routines  ==
;===       Main Menu       ==
;============================
ESPSETUP.EXIT:
    ld  a,CLS
    call    CHPUT
CONFIG_RETURN_TO_EXPLORER:
    CLEAR_UART
    ld  a,CMD_WIFIRELEASE_ESP
    SEND_DATA
    ld  a,CONFIG_RETURN_MENU
    WRITE_F2
    rst #00
    rst #00
ENTERING_ESPSETUP:
    ld  hl,ENTERING_WIFI_SETUP
    call    PRINTHL
    call    PATTERN_SETUP
    call    WAIT_250MS_AND_THEN_CONTINUE
    ; Hold Wi-Fi Connection On
    CLEAR_UART
    SET_SPEED
    ld  a,CMD_WIFIHOLD_ESP
    SEND_DATA
    ld  hl,60                       ; Up to 1s time-out
    ld  a,CMD_WIFIHOLD_ESP
    call    WAIT_MENU_QCMD_RESPONSE ; Wait quick response
    ; We do not want to error here, just in case user still need to upgrade ESP firmware
    ; Whether response ok or hold-fail, fall through to ESPSETUP
ESPSETUP:
    call    RESET_ESP
    or  a
    jp  nz,CONFIG_ESP_NOT_FOUND     ; Not well, ESP was not found
    ; Well, if reset successful, continue
    ld  hl,WELCOME
    call    PRINTHL
    CHECK_QUICK_RECEIVE
    jr  nz,ESPSETUP.2NF             ; If yes, tells Quick Receive support
    ld  hl,WELCOME_SF2
    call    PRINTHL                 ; Print Welcome message no QR
    jr  ESPSETUP.3NF
ESPSETUP.2NF:
    ld  hl,WELCOME_SF
    call    PRINTHL                 ; Print Welcome message QR
ESPSETUP.3NF:
    ld  hl,WELCOME_CS
    call    PRINTHL                 ; Print Wi-Fi is reconnecting to:
ESPSETUP_NEXT:
    call    KILBUF                  ; Clear Keyboard Buffer
    ld  hl,WELCOME_NEXT
    call    PRINTHL
    ld  hl,WELCOME_SF_NEXT
    call    PRINTHL                 ; Print Empty Welcome message
    call    GET_AP_STAT
    jp  z,ESPSETUP_NEXT.1G          ; Won't block in case of error
    ; Success, so connection state is in IX+0, then, zero terminated string with SSID starting at IX+1
    ld  a,(ix+0)
    ld  hl,WELCOME_CS0_NEXT
    cp  0
    jr  z,ESPSETUP_NEXT.2F
    ld  hl,WELCOME_CS1_NEXT
    cp  1
    jr  z,ESPSETUP_NEXT.2F
    ld  hl,WELCOME_CS2_NEXT
    cp  2
    jr  z,ESPSETUP_NEXT.2F
    ld  hl,WELCOME_CS3_NEXT
    cp  3
    jr  z,ESPSETUP_NEXT.2F
    ld  hl,WELCOME_CS4_NEXT
    cp  4
    jr  z,ESPSETUP_NEXT.2F
    ld  hl,WELCOME_CS5_NEXT
ESPSETUP_NEXT.2F:
    ld  iyh,a                       ; Save current conn status in iyh
    call    PRINTHL                 ; Print Status
    push    ix
    pop hl
    inc hl                          ; HL has AP Name
    call    PRINTHLINE              ; Print AP Name
ESPSETUP_NEXT.1G:
    ld  hl,MMENU_S_NEXT
    call    PRINTHL                 ; Print Main Menu
MM_CURSOR_BLINK:
    call    MM_CURSOR_SW            ; Cursor On
CONN_CHG_LOOP:
    call    CHSNS
    jr  nz,MM_WAIT_INPUT            ; If not zero, there is a key in buffer
    ; Ok, there is not, we can check if connection status changed
    call    GET_AP_STAT
    jp  z,CONN_CHG_LOOP             ; Won't block in case of error
    ; Success, so connection state is in IX+0
    ld  a,(ix+0)
    cp  iyh                         ; If same, zero
    jr  z,CONN_CHG_LOOP             ; Keep loop
    call    MM_CURSOR_SW            ; Cursor Off
    jp  ESPSETUP_NEXT               ; Changed, re-build menu
MM_WAIT_INPUT:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP.EXIT             ; When done, resume initialization
    cp  '1'                         ; Setup Nagle?
    jp  z,SET_NAGLE
    cp  '2'                         ; Setup Wi-Fi On Period?
    jp  z,SET_WIFI_TIMEOUT
    cp  '3'                         ; Scan networks?
    jp  z,START_WIFI_SCAN
    cp  '4'                         ; Automatic Setting of Clock?
    jp  z,START_CLK_AUTO
    call    BEEP                    ; Wrong Input, beep
    call    MM_CURSOR_SW            ; Cursor Off
    halt
    jp  MM_CURSOR_BLINK             ; And return, waiting another key
MM_CURSOR_SW:                       ; Cursor workaround for CONN_CHG_LOOP
    ld  a,GOLEFT
    call    CHPUT
    ld  a,1
    ld  (CSRSW),a
    ld  a,' '
    call    CHPUT                   ; Switch the cursor display mode
    xor a
    ld  (CSRSW),a
    ret
GET_AP_STAT:                        ; Use 'call GET_AP_STAT'
    CLEAR_UART
    ld  a,CMD_AP_STS
    SEND_DATA                       ; Get AP conn status and name
    ld  hl,60                       ; Wait Up To 1s
    ld  de,(TXTTAB)                 ; We will borrow Basic Program memory area for now...
    ld  ixl,e
    ld  ixh,d                       ; Address in IX
    jp  WAIT_MENU_CMD_RESPONSE

CONFIG_ESP_NOT_FOUND:
    ld  a,b
    or  a
    ld  hl,FAIL_S                   ; If 0, non responsive
    jr  z,CONFIG_ESP_NOT_FOUND_MSG
    ld  hl,FAIL_F                   ; Otherwise, firmware is old
CONFIG_ESP_NOT_FOUND_MSG:
    call    PRINTHL
    ld  a,180                       ; 3s
    call    WAIT_BEFORE_CONTINUING
    jp  CONFIG_RETURN_TO_EXPLORER

;============================
;===  SETUP Menu Routines  ==
;===  Wi-Fi and Clock Menu ==
;============================
CLK_MSX1_GO:
    ld  hl,MMENU_CLOCK_MSX1
    call    PRINTHL                 ; Print Main Clock MSX1 message
    call    ISCLKAUTO
    ld  a,(ix+0)                    ; Auto Clock Current setting
    cp  3                           ; If 3 adapter disabled
    jr  z,CLK_MSX1_ADAPTERDIS
    ld  hl,MMENU_CLOCK_0
    call    PRINTHL
    jr  CLK_MSX1_OPT
CLK_MSX1_ADAPTERDIS:
    ld  hl,MMENU_CLOCK_3
    call    PRINTHL
CLK_MSX1_OPT:
    ld  hl,MMENU_CLOCK_OPT
    call    PRINTHL
CLK_MSX1_WAIT_OPT_INPUT:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; Back to main menu
    cp  '0'
    jp  z,CLK_MSX1_SEND_CMD
    cp  '3'
    jp  z,CLK_AUTO_WAIT_GMT
    call    BEEP                    ; Wrong Input, beep
    jp  CLK_MSX1_WAIT_OPT_INPUT     ; And return, waiting another key
CLK_MSX1_SEND_CMD:
    call    CHPUT                   ; Print option
    sub '0'                         ; Adjust format
    ld  (ix+0),a                    ; Save it
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    jp  CLK_AUTO_GMT_CHK_DONE       ; And sending the command will be done there

START_CLK_AUTO:
    call    WAIT_250MS_AND_THEN_CONTINUE
    ld  a,(#002D)                   ; Check MSX Version
    or  a
    jp  z,CLK_MSX1_GO               ; If zero, MSX1, so can just enable or disable adapter
CLK_AUTO_GO:
    ld  hl,MMENU_CLOCK_MSX2
    call    PRINTHL                 ; Print Main Clock MSX2 message
    call    ISCLKAUTO
    ld  a,(ix+0)                    ; Auto Clock Current setting
    or  a                           ; If zero, off
    jr  nz,CLK_AUTO_CHK1
    ld  hl,MMENU_CLOCK_0
    call    PRINTHL
    jr  CLK_AUTO_GMT_OPT
CLK_AUTO_CHK1:
    dec a                           ; If 1, on and keep wifi on
    jr  nz,CLK_AUTO_CHK2
    ld  hl,MMENU_CLOCK_1
    call    PRINTHL
    jr  CLK_AUTO_GMT
CLK_AUTO_CHK2:
    dec a                           ; If 2, on and turn wifi off
    jr  nz,CLK_AUTO_3
    ld  hl,MMENU_CLOCK_2
    call    PRINTHL
    jr  CLK_AUTO_GMT
CLK_AUTO_3:
    ld  hl,MMENU_CLOCK_3
    call    PRINTHL
    jr  CLK_AUTO_GMT_OPT
CLK_AUTO_GMT:
    ld  h,(ix+1)                    ; Save it for now
    ld  a,(ix+1)                    ; GMT current setting
    bit 7,a                         ; If set, is -
    jr  z,CLK_AUTO_GMTP
    ld  a,'-'
    call    CHPUT
    res 7,(ix+1)                    ; Clear - indicator
    jr  CLK_AUTO_GMTM
CLK_AUTO_GMTP:
    ld  a,'+'
    call    CHPUT
CLK_AUTO_GMTM:
    ld  a,9
    cp  (ix+1)                      ; Greater than 9?
    jr  nc,CLK_AUTO_GMTD            ; If not, just print what is in A + '0'
    ld  a,'1'                       ; It is 1
    call    CHPUT
    ld  a,(ix+1)
    add '0'-10                      ; Need to subtract 10 and add '0' to print
    call    CHPUT
    jr  CLK_AUTO_GMT_OPT
CLK_AUTO_GMTD:
    ld  a,'0'
    add a,(ix+1)                    ; Our value
    call    CHPUT
    ld  (ix+1),h                    ; Restore original value
CLK_AUTO_GMT_OPT:
    ld  hl,MMENU_CLOCK_OPT
    call    PRINTHL
CLK_AUTO_WAIT_OPT_INPUT:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; Back to main menu
    cp  '0'
    jp  z,CLK_AUTO_WAIT_GMT
    cp  '1'
    jp  z,CLK_AUTO_WAIT_GMT
    cp  '2'
    jp  z,CLK_AUTO_WAIT_GMT
    cp  '3'
    jp  z,CLK_AUTO_WAIT_GMT
    call    BEEP                    ; Wrong Input, beep
    jp  CLK_AUTO_WAIT_OPT_INPUT     ; And return, waiting another key
CLK_AUTO_WAIT_GMT:
    call    CHPUT                   ; Print option
    sub '0'                         ; Adjust format
    ld  (ix+0),a                    ; Save it
    or  a
    jp  z,CLK_AUTO_GMT_CHK_DONE     ; And send the command if just disabling clock auto set
    cp  3
    jp  z,CLK_AUTO_GMT_CHK_DONE     ; Or send the command if just disabling the adapter
    ld  hl,MMENU_GMT_OPT
    call    PRINTHL
    ld  d,0                         ; # of digits entered
    ld  e,0                         ; # of characters printed
    ld  (ix+1),0                    ; GMT 0
CLK_AUTO_WAIT_GMT_INPUT:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; Back to main menu
    cp  #0d                         ; ENTER?
    jp  z,CLK_AUTO_GMT_CHK_INPUT    ; Check if ok to send command
    cp  #08                         ; Backspace?
    jp  z,CLK_AUTO_GMT_CHK_BS       ; Check if there is something to erase
    cp  '-'                         ; Negative value?
    jp  z,CLK_AUTO_GMT_CHK_INPUT
    cp  '0'                         ; >=0?
    jp  c,CLK_AUTO_GMT_BAD_INPUT    ; If not, bad input
    cp  '9'+1                       ; <= 9
    jp  nc,CLK_AUTO_GMT_BAD_INPUT   ; If not, bad input
    jp  CLK_AUTO_GMT_CHK_INPUT      ; Otherwise, validate digit
CLK_AUTO_GMT_CHK_BS:
    xor a
    cp  e                           ; Anything on screen?
    jr  z,CLK_AUTO_GMT_BAD_INPUT    ; Nothing to erase
    dec e                           ; One less character on the screen
    cp  d                           ; Any digit?
    jr  z,CLK_AUTO_GMT_CHK_BS_MIN   ; If not, just erase - sign
    ; There is, so it is one less digit
    dec d
    jr  CLK_AUTO_GMT_CHK_DIGIT
CLK_AUTO_GMT_CHK_BS_MIN:
    ld  (ix+1),0                    ; Reset sign
CLK_AUTO_GMT_CHK_DIGIT:
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    ld  a,' '                       ; Space
    call    CHPUT                   ; Print it
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    jp  CLK_AUTO_WAIT_GMT_INPUT     ; Return
CLK_AUTO_GMT_BAD_INPUT:
    ; Beep might use sub rom, and if so, all registers will be messed up, better save
    push    bc
    push    de
    push    af
    push    hl
    call    BEEP                    ; Wrong Input, beep
    pop hl
    pop af
    pop de
    pop bc
    jp  CLK_AUTO_WAIT_GMT_INPUT     ; And return, waiting another key
CLK_AUTO_GMT_CHK_INPUT:
    ld  c,a                         ; Save in C for printing if needed
    cp  '-'                         ; - sign?
    jr  nz,CLK_AUTO_GMT_CHK_CR      ; If not, check if enter
    ; - sign
    ld  a,0
    cp  e                           ; If not the first character, can't accept
    jr  nz,CLK_AUTO_GMT_BAD_INPUT
    ; It is the first, so let's print it
    ld  a,c
    call    CHPUT
    ld  (ix+1),0x80                 ; Set the - sign bit, for now rest is zero
    inc e                           ; Increase # of characters printed
    jp  CLK_AUTO_WAIT_GMT_INPUT     ; Continue waiting input
CLK_AUTO_GMT_CHK_CR:
    cp  #0d                         ; ENTER?
    jr  nz,CLK_AUTO_GMT_CHK_CD      ; If not, check if digit is valid
    ; Enter
    ld  a,0
    cp  d                           ; Ok, at least one digit entered?
    jr  z,CLK_AUTO_GMT_BAD_INPUT    ; No, so, enter is no good now
    ; It is, if it had a digit entered and did not send, it was 1, so...
    ld  a,1
    or  a,(ix+1)                    ; Adjust sign, if needed
    ld  (ix+1),a                    ; Save
    jr  CLK_AUTO_GMT_CHK_DONE       ; Ok, ready to send
CLK_AUTO_GMT_CHK_CD:
    ; Ok, it is a digit
    ld  b,'0'
    sub a,b                         ; A has digit value
    ld  b,a                         ; Save in B
    ld  a,0
    cp  d
    jr  nz,CLK_AUTO_GMT_CHK_CSD     ; If not zero, it is second digit, so almost done
    ; 1st digit, let's check if it is other than 1, if it is, we are almost done
    ld  a,1
    cp  b
    jr  z,CLK_AUTO_GMT_CHK_CD.1     ; If it is 1, wait next digit or enter
    ; Not 1, so just adjust ix+1 and go
    ld  a,c
    call    CHPUT                   ; Print it
    ld  a,b
    or  a
    jr  z,CLK_AUTO_SKIP_SIGN
    or  a,(ix+1)                    ; Adjust sign, if needed
CLK_AUTO_SKIP_SIGN:
    ld  (ix+1),a                    ; Save
    jr  CLK_AUTO_GMT_CHK_DONE       ; Ok, ready to send
CLK_AUTO_GMT_CHK_CD.1:
    ld  a,c
    call    CHPUT                   ; Print it
    inc d                           ; Digits entered now is 1
    inc e                           ; Digits printed increased
    jp  CLK_AUTO_WAIT_GMT_INPUT     ; Continue waiting input
CLK_AUTO_GMT_CHK_CSD:
    ; Second digit, easy... First was 1, now need to check if it is 0, 1 or 2, otherwise, bad entry
    ld  a,b
    ld  b,3
    cp  b
    jp  nc,CLK_AUTO_GMT_BAD_INPUT   ; 3 or more, so, not valid
    or  a                           ; Is it zero?
    jr  nz,CLK_AUTO_GMT_CHK_CSD1    ; If not, check for 1
    ; It was zero
    ld  a,10                        ; So, 10
    or  a,(ix+1)                    ; Adjust sign, if needed
    ld  (ix+1),a                    ; Save
    ld  a,c
    call    CHPUT                   ; Print it
    jr  CLK_AUTO_GMT_CHK_DONE       ; Ready to
CLK_AUTO_GMT_CHK_CSD1:
    dec a                           ; Is it one?
    jr  nz,CLK_AUTO_GMT_CHK_CSD2
    ; It was one
    ld  a,11                        ; So, eleven
    or  a,(ix+1)                    ; Adjust sign, if needed
    ld  (ix+1),a                    ; Save
    ld  a,c
    call    CHPUT                   ; Print it
    jr  CLK_AUTO_GMT_CHK_DONE       ; Ready to send
CLK_AUTO_GMT_CHK_CSD2:
    ; It was two
    ld  a,12                        ; So, twelve
    or  a,(ix+1)                    ; Adjust sign, if needed
    ld  (ix+1),a                    ; Save
    ld  a,c
    call    CHPUT                   ; Print it
    ; And send command
CLK_AUTO_GMT_CHK_DONE:
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    ld  hl,STR_SENDING
    call    PRINTHL
    CLEAR_UART
    ld  a,CMD_SET_ACLK_SETTINGS
    SEND_DATA
    ld  a,0                         ; Size MSB is 0
    SEND_DATA
    ld  a,2                         ; Size LSB is 2
    SEND_DATA
    ld  a,(ix+0)                    ; Option
    SEND_DATA
    ld  a,(ix+1)                    ; GMT
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    ld  a,CMD_SET_ACLK_SETTINGS
    call    WAIT_MENU_QCMD_RESPONSE
    ld  hl,STR_SENDING_OK
    jr  nz,CLK_AUTO_GMT_CHK_RESULT
    ld  hl,STR_SENDING_FAIL
CLK_AUTO_GMT_CHK_RESULT:
    call    PRINTHL
    jp  WAIT_2S_AND_THEN_MAINMENU

;============================
;===  SETUP Menu Routines  ==
;===    Wi-Fi Scan Menu    ==
;============================
START_WIFI_SCAN:
    call    WAIT_250MS_AND_THEN_CONTINUE
START_WIFI_RESCAN:
    ld  hl,MMENU_SCAN
    call    PRINTHL                 ; Print Main Scan message
    call    STARTWIFISCAN           ; Request Wi-Fi Scan to start
    ld  de,(TXTTAB)                 ; We will borrow Basic Program memory area for now...
    ld  ixl,e
    ld  ixh,d                       ; Address in IX
    ld  de,20                       ; At least 10s waiting scan to finish, retry 20 times waiting 0.5s between attempts
WIFI_SCAN_WAIT_END:
    CLEAR_UART
    ld  a,CMD_SCAN_RESULTS
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    call    WAIT_MENU_SCMD_RESPONSE
    jp  nz,WIFI_SCAN_SHOW_LIST      ; if success show the list
    ld  a,b
    cp  2                           ; If 2, scan is done and nothing found
    jp  z,WIFI_SCAN_NONETWORKS
    ld  l,30
WIFI_SCAN_WAITHS:
    halt
    dec l
    jr  nz,WIFI_SCAN_WAITHS
    dec de
    ld  a,e
    or  d
    jp  z,WIFI_SCAN_TIMEOUT
    jp  WIFI_SCAN_WAIT_END
WIFI_SCAN_SHOW_LIST:
    ld  d,a                         ; Save access point counter here
    ld  e,0                         ; And here how many were printed
    ld  hl,MMENU_SCANS
    call    PRINTHL
    push    ix
    pop hl                          ; IX in HL
WIFI_LIST_LOOP:
    ld  a,e
    add a,'0'                       ; Convert to number
    call    CHPUT
    ld  a,' '                       ; Space
    call    CHPUT
    ld  a,'-'                       ; Dash
    call    CHPUT
    ld  a,' '                       ; Space
    call    CHPUT
    ld  b,24                        ; Cuts AP names > 23 chars
PRT_APNAMELP:
    ld  a,(hl)
    or  a
    jp  z,PRT_APENC
    call    CHPUT
    inc hl
    djnz    PRT_APNAMELP
    push    hl
    ld  hl,SCAN_TERMINATOR_CUT
    call    PRINTHL
    pop hl
PRT_APNAMELP_CUT:
    ld  a,(hl)
    or  a
    jp  z,PRT_APENC
    inc hl
    jp  PRT_APNAMELP_CUT
PRT_APENC:
    inc hl
    ld  a,(hl)
    or  a
    jp  z,PRT_APNOTENC
    push    hl
    ld  hl,SCAN_TERMINATOR_ENC
    call    PRINTHL
    pop hl
    jp  PRT_AP_CHKLOOP
PRT_APNOTENC:
    push    hl
    ld  hl,SCAN_TERMINATOR_OPEN
    call    PRINTHL
    pop hl
PRT_AP_CHKLOOP:
    inc hl
    inc e
    ld  a,SCAN_MAX_PAGE_SIZE
    cp  e
    jp  z,APLIST_OVERFLOW
    dec d
    jp  nz,WIFI_LIST_LOOP
    ld  b,0                         ; Signal no more list data
    jr  APLIST_NO_OVERFLOW
APLIST_OVERFLOW:
    dec d                           ; Update remaining items
    xor a
    or  d                           ; Still has items?
    jr  z,APLIST_NO_OVERFLOW        ; No more items
    ld  b,1                         ; Signal that there still is data pending to list in another page
    push    hl
    pop iy                          ; Save in IY the address to continue from
    ld  c,d                         ; And C has the remaining AP count
    ld  hl,MMENU_SCANQM
    jr  APLIST_NOFLW
APLIST_NO_OVERFLOW:
    ; If here, current page has been printed
    ; E has the maximum allowable AP number
    ; B will indicate if there are more items for a next page
    ; IY will hold the address of the following items
    ; Let's ask which one to connect
    ld  hl,MMENU_SCANQ
APLIST_NOFLW:
    call    PRINTHL                 ; Show message asking which network to connect
    ld  a,'0'
    add a,e
    ld  e,a                         ; To make it easy in the selection screen
WIFI_SELECT_AP:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; When done, back to main setup
    cp  #20                         ; Spacebar?
    jp  z,WIFI_SELECT_SPACEBAR      ; Check if re-scan or next page
    cp  CR                          ; ENTER?
    jp  z,WIFI_CONNECT_ME_CHOOSEN   ; Request AP Name and Password if needed
    cp  '0'                         ; Check if A is less than 0
    jp  c,INPUT_WFSAP_BAD_INPUT     ; If it is, ignore
    cp  e                           ; Check if a is greater than what is in E
    jp  nc,INPUT_WFSAP_BAD_INPUT    ; If it is, ignore
    jp  WIFI_CONNECT_SELECTION_OK   ; Selection Ok if here

WIFI_SELECT_SPACEBAR:
    xor a
    or  d                           ; D = AP counter
    jp  z,START_WIFI_RESCAN         ; Rescan if no more items
    ; Otherwise, more items, start at IY
    push    iy
    pop ix                          ; Restore the list from where we finished last time
    ld  a,c                         ; Restore the remaining APs to list
    jp  WIFI_SCAN_SHOW_LIST         ; And show

WIFI_CONNECT_ME_CHOOSEN:
    ld  hl,MMENU_MANUALENTRY
    call    PRINTHL                 ; Show message asking AP details....
    ; IX has the AP list, we don't care, but that address is ours to use to handle SSID Entry
    push    ix
    pop hl                          ; HL has the address
    ld  c,0                         ; AP name size
WIFI_CONNECT_MANUAL_ENTRY:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; Back to main setup
    cp  #08                         ; Backspace?
    jr  z,WIFI_CONNECT_ME_BS        ; Check if there is something to erase
    cp  #0d                         ; ENTER?
    jr  z,WIFI_ME_CHECK_INPUT       ; Check if ok
    ; Otherwise it is a digit
    ld  (hl),a
    ld  a,c
    sub 32                          ; SSID maximum is 32 characters
    jr  z,WIFI_CONNECT_MANUAL_ENTRY
    ld  a,(hl)
    inc c
    inc hl
    cp  32
    jr  nc,WIFI_CONNECT_ME_CHKPRT2  ; Ok, not below space, but is it delete?
    ld  a,'?'                       ; Prints a question mark
    jr  WIFI_CONNECT_ME_CHKPRTD
WIFI_CONNECT_ME_CHKPRT2:
    cp  #7f
    jr  nz,WIFI_CONNECT_ME_CHKPRTD
    ld  a,'?'                       ; Prints a question mark if it is delete
WIFI_CONNECT_ME_CHKPRTD:
    call    CHPUT                   ; Print a char
    jp  WIFI_CONNECT_MANUAL_ENTRY
WIFI_CONNECT_ME_0_TERM:
    xor a
    ld  (hl),a
    inc hl
    push    hl                      ; Save it
    ld  hl,MENU_MANUALENTRY_PWD
    call    PRINTHL                 ; We are asking if password is needed
    pop hl                          ; Restore it
    call    CHGET                   ; Get key, if Y or y, needed, otherwise not
    res 5,a                         ; Force upper case
    cp  'Y'
    jr  z,WIFI_CONNECT_ME_PWD_Y     ; And tell using encryption
    ld  a,'n'
    call    CHPUT                   ; Overwrite what was typed with 'n'
    xor a                           ; Ok, if here, no pwd, so no encryption
    jr  WIFI_CONNECT_ME_PWD
WIFI_CONNECT_ME_PWD_Y:
    ld  a,'y'
    call    CHPUT                   ; Print 'y'
    ld  a,1                         ; Do not change flags, pwd needed
WIFI_CONNECT_ME_PWD:
    ld  (hl),a                      ; Save if encryption is expected
    push    ix
    pop hl                          ; HL containing the beginning of SSID name
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    jp  WIFI_CONNECT_AP_PWDQ        ; And the rest is handled like if menu selected
WIFI_CONNECT_ME_BS:
    xor a
    cp  c                           ; Has any data to delete?
    jr  z,WIFI_CONNECT_MANUAL_ENTRY ; No
    ; Yes
    dec hl                          ; Decrement pointer
    dec c                           ; Decrement counter
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    ld  a,' '                       ; Space
    call    CHPUT                   ; Print it
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    jr  WIFI_CONNECT_MANUAL_ENTRY   ; Return
WIFI_ME_CHECK_INPUT:
    xor a
    cp  c                           ; Has received any data?
    jr  z,WIFI_CONNECT_MANUAL_ENTRY ; No
    jp  WIFI_CONNECT_ME_0_TERM      ; Yes, so handle SSID input termination

WIFI_CONNECT_SELECTION_OK:
    call    CHPUT                   ; Valid input, print it
    ld  e,'0'
    sub a,e                         ; Get in decimal
    ld  e,a                         ; Back in E
    ; IX has the AP list, A which one has been selected, now our routine will do it
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    push    ix
    pop hl                          ; put IX in HL
WIFI_CONNECT_AP_SRCH:
    ld  a,e
    or  a
WIFI_CONNECT_AP_SRCH.1:
    jp  z,WIFI_CONNECT_AP_PWDQ
    ld  a,(hl)
    inc hl
    or  a
    jp  nz,WIFI_CONNECT_AP_SRCH.1   ; Find string terminator
    ; Found, jump encryption byte
    inc hl
    dec e                           ; Decrement selection, if 0 we are done
    jp  nz,WIFI_CONNECT_AP_SRCH.1
WIFI_CONNECT_AP_PWDQ:
    ; HL has the address of AP name string
    ld  d,h
    ld  e,l                         ; Save copy in D
    ld  bc,0                        ; BC will have the ap connection data length
WIFI_CONNECT_APSIZE:
    inc bc
    ld  a,(de)
    inc de
    or  a
    jp  nz,WIFI_CONNECT_APSIZE      ; Count size, including zero terminator
    ; Check for encryption
    ld  a,(de)
    or  a
    jp  z,WIFI_CONNECT_SENDCMD      ; If no password requested, good to go
    ; Shoot, need to request password, well, let's do it
    push    hl                      ; Save HL
    ld  hl,MMENU_ASKPWD
    call    PRINTHL                 ; Inform that user need to input PWD
    pop hl                          ; Restore HL
    ld  iy,0                        ; IY will help in backspacing
    ld  ixh,1                       ; Start hidden
WIFI_CONNECT_RCV_PWD:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; When done, back to main setup
    cp  #08                         ; Backspace?
    jp  z,WIFI_CONNECT_RCV_PWD_BS   ; Check if there is something to erase
    cp  #0d                         ; ENTER?
    jp  z,WIFI_PWD_CHECK_INPUT      ; Check if ok
    cp  #7f                         ; Delete?
    jp  z,WIFI_CONNECT_RCV_PWDH     ; Change password from clear to hidden or vice versa
WIFI_CONNECT_RCV_PWD_STR:
    ; Ok, so it is a digit and store
    ld  (de),a
    ld  a,iyl
    sub 63                          ; 63 chars password limit (WPA2 encryption)
    jr  z,WIFI_CONNECT_RCV_PWD
    ld  a,(de)
    inc bc
    inc de
    inc iy                          ; Increment counters and pointer
    push    af                      ; Save A
    ld  a,ixh
    or  a                           ; If zero, print char, otherwise print *
    jr  z,WIFI_CONNECT_RCV_PWD_CHAR
    pop af
    ld  a,'*'                       ; Print * and keep password hidden
    call    CHPUT
    jp  WIFI_CONNECT_RCV_PWD        ; And back to receiving digits
WIFI_CONNECT_RCV_PWD_CHAR:
    pop af
    cp  32
    jr  nc,WIFI_CONNECT_RCV_CHKPRT2 ; Ok, not below space, but is it delete?
    ld  a,'?'                       ; Prints a question mark
    jr  WIFI_CONNECT_RCV_CHKPRTD
WIFI_CONNECT_RCV_CHKPRT2:
    cp  #7f
    jr  nz,WIFI_CONNECT_RCV_CHKPRTD
    ld  a,'?'                       ; Prints a question mark if it is delete
WIFI_CONNECT_RCV_CHKPRTD:
    call    CHPUT                   ; Print a char
    jp  WIFI_CONNECT_RCV_PWD        ; And back to receiving digits

WIFI_CONNECT_RCV_PWDH:
    ld  a,iyl
    or  iyh
    jp  nz,WIFI_CONNECT_RCV_PWD_STR ; If digits entered, can't change password behavior, so it is a pass phrase char
    xor a
    or  ixh
    ld  ixh,1
    jr  z,WIFI_CONNECT_RCV_PWD      ; Return if it was 0
    ld  ixh,0
    jr  z,WIFI_CONNECT_RCV_PWD      ; Otherwise set to 0 and return

WIFI_CONNECT_RCV_PWD_BS:
    ld  a,iyl
    or  iyh
    jp  z,WIFI_CONNECT_RCV_PWD      ; If no digits entered, nothing to erase
    dec iy                          ; Decrement counter
    dec bc                          ; Decrement counter
    dec de                          ; Decrement pointer
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    ld  a,' '                       ; Space
    call    CHPUT                   ; Print it
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    jp  WIFI_CONNECT_RCV_PWD        ; Return

WIFI_PWD_CHECK_INPUT:
    ld  a,iyl
    or  iyh
    jp  z,WIFI_CONNECT_RCV_PWD      ; If no digits entered, no password to send
    ; Otherwise done and ready to send
WIFI_CONNECT_SENDCMD:
    call    WAIT_250MS_AND_THEN_CONTINUE
    push    hl
    ld  HL,MMENU_CONNECTING
    call    PRINTHL
    pop hl
    ; Print AP name
    push    hl
    call    PRINTHLINE              ; Print AP Name
    pop hl
    ; HL has the address of our data, BC the data size, so it is just needed to send the command
    CLEAR_UART
    ld  a,CMD_WIFI_CONNECT
    SEND_DATA
    ld  a,b                         ; Size MSB is in B
    SEND_DATA
    ld  a,c                         ; Size LSB is in c
    SEND_DATA
WIFI_CONNECT_SENDCMDLP:
    ld  a,(hl)
    SEND_DATA
    inc hl
    dec bc
    ld  a,b
    or  c
    jp  nz,WIFI_CONNECT_SENDCMDLP
    ld  hl,600                      ; Wait Up To 10s
    ld  a,CMD_WIFI_CONNECT          ; Our command
    call    WAIT_MENU_QCMD_RESPONSE
    jp  z,WIFI_CONNECT_FAIL
    ld  hl,STR_SENDING_OK_JN        ; Success
    call    PRINTHL
    jp  WAIT_2S_AND_THEN_MAINMENU
WIFI_CONNECT_FAIL:
    ld  hl,STR_SENDING_NOK_JN       ; Failure
    call    PRINTHL
    jp  WAIT_4S_AND_THEN_MAINMENU

INPUT_WFSAP_BAD_INPUT:
    push    bc
    push    de
    push    af
    push    hl
    push    ix
    push    iy
    call    BEEP                    ; Wrong Input, beep
    pop iy
    pop ix
    pop hl
    pop af
    pop de
    pop bc
    jp WIFI_SELECT_AP               ; Return

WIFI_SCAN_NONETWORKS:
    ld  hl,MMENU_SCANN
    call    PRINTHL
    jp  WAIT_4S_AND_THEN_MAINMENU

WIFI_SCAN_TIMEOUT:
    ld  hl,MMENU_SCANF
    call    PRINTHL
    jp  WAIT_4S_AND_THEN_MAINMENU

SET_WIFI_TIMEOUT:
    call    WAIT_250MS_AND_THEN_CONTINUE
    ld  hl,MMENU_TIMEOUT
    call    PRINTHL                 ; Print Main Timeout message
    call    CHECKTIMEOUT            ; TimeOut is on or off?
    jp  z,WIFI_SET_ALWAYS_ON        ; If 0, always on
    ; Otherwise there is a timeout
    push    hl
    ld hl,MMENU_TIMEOUT_NOTALWAYSON1
    call    PRINTHL
    pop hl
    call    PRINTHL
    ld hl,MMENU_TIMEOUT_NOTALWAYSON2
    call    PRINTHL
    ld  d,0                         ; Count digits
    jr  INPUT_TIMEOUT
WIFI_SET_ALWAYS_ON:
    ld  hl,MMENU_TIMEOUT_ALWAYSON
    call    PRINTHL
    ld  de,(TXTTAB)                 ; We will borrow Basic Program memory area for now...
    ld  ixl,e
    ld  ixh,d                       ; Address in IX
    ld  d,0                         ; Count digits
INPUT_TIMEOUT:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; When done, back to main setup
    cp  #0d                         ; ENTER?
    jp  z,SET_WIFI_CHECK_INPUT      ; Check if ok
    cp  #08                         ; Backspace?
    jp  z,SET_WIFI_BS_INPUT         ; Check if there is something to erase
    cp  '0'                         ; Check if A is less than 0
    jp  c,INPUT_TIMEOUT_BAD_INPUT   ; if it is, ignore
    cp  '9'+1                       ; Check if a is greater than  9
    jp  nc,INPUT_TIMEOUT_BAD_INPUT  ; If it is, ignore
    ld  (ix+0),a                    ; Save it
    call    CHPUT                   ; It is valid, so print it
    inc d                           ; Increment digit count
    inc ix                          ; Increment pointer
    ld  a,3
    cp  d
    jp  z,SET_WIFI_CHECK_INPUT      ; All we can do is accept up to 3 digits, check if ok
    jp  INPUT_TIMEOUT               ; Not done yet, so continue

INPUT_TIMEOUT_BAD_INPUT:
    call    BEEP                    ; Wrong Input, beep
    jp INPUT_TIMEOUT                ; Return

SET_WIFI_BS_INPUT:
    xor a
    or  d                           ; Counter has any digit?
    jp  z,INPUT_TIMEOUT             ; Nope, so just continue
    dec d                           ; Decrement counter
    dec ix                          ; Decrement pointer
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    ld  a,' '                       ; Space
    call    CHPUT                   ; Print it
    ld  a,8                         ; Backspace
    call    CHPUT                   ; Print it
    jp  INPUT_TIMEOUT               ; Return

SET_WIFI_CHECK_INPUT:
    xor a
    or  d                           ; Counter has any digits
    jp  z,INPUT_TIMEOUT             ; Nope, so just continue
    ; IX is pointing one position after last digit, so revert
    dec ix
    ld  a,(ix+0)                    ; Digit in A
    sub '0'                         ; Convert it to decimal
    ld  h,0                         ; First digit, so H is 0
    ld  l,a                         ; And L has the digit
    dec d                           ; If digits finished, just set
    jp  z,SET_WIFI_EXECUTE_SET_COMMAND
    dec ix
    ld  a,(ix+0)                    ; Digit in A
    sub '0'                         ; Convert it to decimal
    add a,a                         ; A*2
    ld  c,a                         ; A*2 in C
    add a,a                         ; A*4
    add a,a                         ; A*8
    add a,c                         ; A*10
    ; Up to here, we can get 90 + 9, 99, won't go to H anyway, just add to L
    add a,l                         ; L has the first digit
    ld  l,a                         ; And now L has the two digits
    dec d                           ; If digits finished, just set
    jp  z,SET_WIFI_EXECUTE_SET_COMMAND
    dec ix
    ld  a,(ix+0)                    ; Digit in A
    sub '0'                         ; Convert it to decimal
    add a,a                         ; A*2
    ld  c,a                         ; A*2 in C
    add a,a                         ; A*4
    add a,a                         ; A*8
    add a,c                         ; A*10
    ex  de,hl                       ; Get the two digits results in de
    ld  l,a
    ld  h,0                         ; HL = A*10
    add hl,hl                       ; HL = A*20
    ld  c,l
    ld  b,h                         ; BC = A*20
    add hl,hl                       ; HL = A*40
    add hl,hl                       ; HL = A*80
    add hl,bc                       ; HL = A*100
    add hl,de                       ; HL = three digits result
    ; This was the last digit, up to three
SET_WIFI_EXECUTE_SET_COMMAND:
    jp  SET_ESP_WIFI_TIMEOUT        ; And set and done

;============================
;===  SETUP Menu Routines  ==
;===      NAGLE Menu       ==
;============================
SET_NAGLE:
    call    WAIT_250MS_AND_THEN_CONTINUE
    ld  hl,MMENU_NAGLE
    call    PRINTHL                 ; Print Main Nagle message
    call    CHECKNAGLE              ; Nagle is on or off?
    jr  nz,NAGLE_IS_ON              ;
    ld  hl,MMENU_NAGLE_OFF          ; Show the menu telling nagle is off
    call    PRINTHL                 ; Print options
SET_NAGLE_WI_ON:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; When done, back to main setup
    cp  'O'                         ; Toggle Nagle On
    jp  z,SET_NAGLE_ON              ;
    cp  'o'                         ; Toggle Nagle On
    jp  z,SET_NAGLE_ON              ;
    call    BEEP                    ; Wrong Input, beep
    jp  SET_NAGLE_WI_ON             ; And return waiting key

NAGLE_IS_ON:
    ld  hl,MMENU_NAGLE_ON           ; Show the menu telling nagle is on
    call    PRINTHL                 ; Print options
SET_NAGLE_WI_OFF:
    call    CHGET
    cp  #1b                         ; ESC?
    jp  z,ESPSETUP                  ; When done, back to main setup
    cp  'O'                         ; Toggle Nagle Off
    jp  z,SET_NAGLE_OFF             ;
    cp  'o'                         ; Toggle Nagle Off
    jp  z,SET_NAGLE_OFF             ;
    call    BEEP                    ; Wrong Input, beep
    jp  SET_NAGLE_WI_OFF            ; And return waiting key

;============================
;===  SETUP Menu Routines  ==
;===  Auxiliary Functions  ==
;============================
SET_ESP_WIFI_TIMEOUT:
    ex  de,hl
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    ld  hl,STR_SENDING              ; Indicate it is sending a command
    call    PRINTHL
    CLEAR_UART
    ld  a,CMD_TIMER_SET
    SEND_DATA
    xor a                           ; Size MSB is 0
    SEND_DATA
    ld  a,2                         ; Size LSB is 2
    SEND_DATA
    ld  a,d                         ; Timeout MSB
    SEND_DATA
    ld  a,e                         ; Timeout LSB
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    ld  a,CMD_TIMER_SET             ; Our command
    call    WAIT_MENU_QCMD_RESPONSE
    jp  z,MENU_BAD_END
    ld  hl,STR_SENDING_OK           ; Success
    call    PRINTHL
    jp  WAIT_2S_AND_THEN_MAINMENU

SET_NAGLE_OFF:
    ld  a,'O'
    call    CHPUT
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    ld  hl,STR_SENDING              ; Indicate it is sending a command
    call    PRINTHL
    CLEAR_UART
    ld  a,CMD_NAGLE_OFF
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    call    WAIT_MENU_QCMD_RESPONSE
    jp  z,MENU_BAD_END
    ld  hl,STR_SENDING_OK           ; Success
    call    PRINTHL
    jp  WAIT_2S_AND_THEN_MAINMENU

STARTWIFISCAN:
    CLEAR_UART
    ld  a,CMD_SCAN_START
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    call    WAIT_MENU_QCMD_RESPONSE
    ret nz                          ; If success return
    ld  a,CR
    call    CHPUT
    ld  a,LF
    call    CHPUT
    jp  MENU_SUB_BAD_END            ; If error, nothing much to do, main menu

SET_NAGLE_ON:
    ld  a,'O'
    call    CHPUT
    ld  a,#0d
    call    CHPUT
    ld  a,#0a
    call    CHPUT
    ld  hl,STR_SENDING              ; Indicate it is sending a command
    call    PRINTHL
    CLEAR_UART
    ld  a,CMD_NAGLE_ON
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    call    WAIT_MENU_QCMD_RESPONSE
    jp  z,MENU_BAD_END
    ld  hl,STR_SENDING_OK           ; Success
    call    PRINTHL
    jp  WAIT_2S_AND_THEN_MAINMENU

WAIT_4S_AND_THEN_MAINMENU:
    ld  a,240                       ; Wait 4 seconds with message on screen
    jr  WAIT_AND_THEN_MAINMENU
WAIT_2S_AND_THEN_MAINMENU:
    ld  a,120                       ; Wait 2 seconds with message on screen
WAIT_AND_THEN_MAINMENU:
    call    WAIT_BEFORE_CONTINUING
    jp  ESPSETUP                    ; When done, back to main setup
WAIT_250MS_AND_THEN_CONTINUE:
    ld  a,15                        ; Wait 250 ms then continue
WAIT_BEFORE_CONTINUING:
    halt
    dec a
    ; If not zero, our time out has not elapsed
    jr  nz,WAIT_BEFORE_CONTINUING
    ret                             ; Time out and continue

; Check Auto Clock
ISCLKAUTO:
    CLEAR_UART
    ld  a,CMD_QUERY_ACLK_SETTINGS
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    ld  de,(TXTTAB)                 ; We will borrow Basic Program memory area for now...
    ld  ixl,e
    ld  ixh,d                       ; address in IX
    call    WAIT_MENU_CMD_RESPONSE
    jp  z,MENU_SUB_BAD_END
    ; Response received, IX+0 and IX+1 has Auto Clock and GMT, A
    ret

; Check what is the current NAGLE setting
CHECKNAGLE:
    CLEAR_UART
    ld  a,CMD_QUERY_ESP_SETTINGS
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    ld  de,(TXTTAB)                 ; We will borrow Basic Program memory area for now...
    ld  ixl,e
    ld  ixh,d                       ; Address in IX
    call    WAIT_MENU_CMD_RESPONSE
    jp  z,MENU_SUB_BAD_END
    ; Response received, nagle is the first one, ON: or OFF:
    ld  a,'O'
    cp  (ix+0)
    jp  nz,MENU_SUB_BAD_END
    ld  a,'N'
    cp  (ix+1)
    jp  nz,CHECK_NAGLE_OFF
    ld  a,':'
    cp  (ix+2)
    jp  nz,MENU_SUB_BAD_END
    or  a                           ; It will make it NZ
    ret
CHECK_NAGLE_OFF:
    ld  a,'F'
    cp  (ix+1)
    jp  nz,MENU_SUB_BAD_END
    ld  a,'F'
    cp  (ix+2)
    jp  nz,MENU_SUB_BAD_END
    ld  a,':'
    cp  (ix+3)
    jp  nz,MENU_SUB_BAD_END
    ; Already has zero, so just ret
    ret

; Check what is the current TIMEOUT setting, return 0 if always on, otherwise there is a timeout
; Will return a zero terminated string @ HL that can be printed
; Will return the value in DE
CHECKTIMEOUT:
    CLEAR_UART
    ld  a,CMD_QUERY_ESP_SETTINGS
    SEND_DATA
    ld  hl,60                       ; Wait Up To 1s
    ld  de,(TXTTAB)                 ; We will borrow Basic Program memory area for now...
    ld  ixl,e
    ld  ixh,d                       ; Address in IX
    call    WAIT_MENU_CMD_RESPONSE
    jp  z,MENU_SUB_BAD_END
    ; Response received, nagle is the first one, ON: or OFF:
    ld  a,':'
    inc ix
    inc ix                          ; Nagle response is two or three bytes long, let's check
    dec bc
    dec bc                          ; Remaining bytes
    cp  (ix+0)
    jp  z,CHECKTIMEOUT.1
    inc ix
    dec bc                          ; Remaining bytes
    cp  (ix+0)
    jp  nz,MENU_SUB_BAD_END         ; If not here, sorry to say it is an error
CHECKTIMEOUT.1:
    ld  a,b
    or  c                           ; All data read?
    jp  z,MENU_SUB_BAD_END          ; If so, sorry to say it is an error
    inc ix                          ; At the first digit
    dec bc                          ; Remaining bytes
    ld  a,b
    or  c                           ; All data read?
    jp  z,MENU_SUB_BAD_END          ; If so, sorry to say it is an error
    push    ix                      ; This is the start of the string, save it
    ld  h,0                         ; No digit so far
    ; It can have up to three digits
CHECKTIMEOUT.2:
    ld  a,':'
    cp  (ix+0)                      ; Check if it is the separator
    jp  z,CHECKTIMEOUT.3            ; If it is routine will follow through
    ld  a,(ix+0)                    ; Get the supposed digit in A
    ld  l,'9'+1
    cp  l
    jp  nc,MENU_SUB_BAD_END_1S      ; If more than '9', sorry to say it is an error
    ld  l,'0'
    cp  l
    jp  c,MENU_SUB_BAD_END_1S       ; If less than '0', sorry to say it is an error
    inc h                           ; It is not, so it is a digit
    ld  a,3
    cp  h
    jp  c,MENU_SUB_BAD_END_1S       ; If more than three digits, sorry to say it is an error
    inc ix                          ; Increase pointer
    dec bc                          ; Decrease remaining
    ld  a,b
    or  c                           ; All data read?
    jp  nz,CHECKTIMEOUT.2           ; Not, so rinse and repeat
CHECKTIMEOUT.3:
    ld  (ix+0),0                    ; Null terminate string value
    dec ix
    ld  a,(ix+0)                    ; 1st Digit in A
    sub '0'                         ; Convert it to decimal value
    ld  e,a
    ld  d,0                         ; DE has first digit
    dec h                           ; Decrement digit counter
    jr  z,CHECKTIMEOUT.END          ; If all digits, done
    ; Now second digit, multiply it by 10 and add to E, even if 90 + 9, still fits E
    dec ix
    ld  a,(ix+0)                    ; 2nd Digit
    sub '0'                         ; Convert it to decimal value
    add a,a                         ; A has *2
    ld  c,a                         ; C has *2
    add a,a                         ; A has *4
    add a,a                         ; A has *8
    add a,c                         ; A has *10
    add a,e                         ; A has two digits result
    ld  e,a                         ; Back to E, DE has two digits results
    dec h                           ; Decrement digit counter
    jr  z,CHECKTIMEOUT.END          ; If all digits, done
    ; Now Third digit, multiply it by 100 and add to DE
    dec ix
    ld  a,(ix+0)                    ; 3rd Digit
    sub '0'                         ; Convert it to decimal value
    add a,a                         ; A has *2
    ld  c,a                         ; C has *2
    add a,a                         ; A has *4
    add a,a                         ; A has *8
    add a,c                         ; A has *10
    ld  h,0
    ld  l,a                         ; HL has *10
    add hl,hl                       ; HL has *20
    ld  b,h
    ld  c,l                         ; BC has *20
    add hl,hl                       ; HL has *40
    add hl,hl                       ; HL has *80
    add hl,bc                       ; HL has *100
    add hl,de                       ; HL has three digit value
    ex  de,hl                       ; now in DE
CHECKTIMEOUT.END:
    pop hl                          ; Restore address of string version of time count
    ld  a,d
    or  e                           ; Set zero flag according to the time out set
    ret

MENU_SUB_BAD_END_1S:
    pop af                          ; 1 register was stacked, pop it
MENU_SUB_BAD_END:
    pop af                          ; It was a sub, so clear stack
MENU_BAD_END:
    ld  hl,STR_SENDING_FAIL         ; error message
    call    PRINTHL
    jp  WAIT_2S_AND_THEN_MAINMENU

; WAIT an ESP quick command Response
; Inputs:
; A -> Command Code
; HL -> Timeout
;
; Returns:
; Flag Z is zero if failure, non zero if success
;
; Affect:
; AF and HL
;
WAIT_MENU_QCMD_RESPONSE:
    push    de
    ld  d,a                         ; Command to wait in D
WAIT_MENU_QCMD_RESPONSE_ST1:
    LOAD_STS_PORT_IN_A
    bit 5,a                         ; If nz Port #07 is not available
    jp  nz,WAIT_MENU_QCMD_RESPONSE_END_NOK
    bit 0,a                         ; If nz has data
    jr  nz,WAIT_MENU_QCMD_RESPONSE_ST1.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_QCMD_RESPONSE_ST1
    jp  WAIT_MENU_QCMD_RESPONSE_END ; If time out waiting, return
WAIT_MENU_QCMD_RESPONSE_ST1.1:
    ; nz, check the data
    RECEIVE_DATA
    cp  d                           ; Is response of our command?
    jr  z,WAIT_MENU_QCMD_RESPONSE_RC
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_QCMD_RESPONSE_ST1
    jp  WAIT_MENU_QCMD_RESPONSE_END ; Give up if unexpected bytes keep arriving until timeout
    ; Now get return code, if return code other than 0, it is failure, otherwise success
WAIT_MENU_QCMD_RESPONSE_RC:
    CHECK_DATA
    jr  nz,WAIT_MENU_QCMD_RESPONSE_RC.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_QCMD_RESPONSE_RC
    jp  WAIT_MENU_QCMD_RESPONSE_END ; If time-out waiting, return
WAIT_MENU_QCMD_RESPONSE_RC.1:
    RECEIVE_DATA
    or  a                           ; 0?
    ; If not, done
    jp  nz,WAIT_MENU_QCMD_RESPONSE_END_NOK
WAIT_MENU_QCMD_RESPONSE_END_OK:
    ld  a,1
    or  a                           ; NZ to indicate success
WAIT_MENU_QCMD_RESPONSE_END:
    pop de
    ret
WAIT_MENU_QCMD_RESPONSE_END_NOK:
    ld  hl,0
    xor a
    pop de
    ret

; WAIT an ESP regular command Response
; Inputs:
; A -> Command Code
; HL -> Timeout
; IX -> Where to store response
;
; Returns:
; Flag Z is zero if failure, non zero if success
; BC is the response size
;
; Affect:
; AF , BC and HL
WAIT_MENU_CMD_RESPONSE:
    push    de
    push    ix
    ld  d,a                         ; Command to wait in D
WAIT_MENU_CMD_RESPONSE_ST1:
    LOAD_STS_PORT_IN_A
    bit 5,a                         ; If nz Port #07 is not available
    jp  nz,WAIT_MENU_CMD_RESPONSE_END_NOK
    bit 0,a                         ; If nz has data
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST1.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST1
    jp  WAIT_MENU_CMD_RESPONSE_END  ; If time out waiting, return
WAIT_MENU_CMD_RESPONSE_ST1.1:
    ; nz, check the data
    RECEIVE_DATA
    cp  d                           ; Is response of our command?
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST1
    ; Now get return code, if return code other than 0, it is finished
WAIT_MENU_CMD_RESPONSE_RC:
    CHECK_DATA
    jr  nz,WAIT_MENU_CMD_RESPONSE_RC.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_CMD_RESPONSE_RC
    jp  WAIT_MENU_CMD_RESPONSE_END  ; If time out waiting, return
WAIT_MENU_CMD_RESPONSE_RC.1:
    RECEIVE_DATA
    or  a                           ; 0?
    ; If not, done
    jp  nz,WAIT_MENU_CMD_RESPONSE_END_NOK
    ; Next two bytes are size bytes, save it to BC
WAIT_MENU_CMD_RESPONSE_ST2A:
    CHECK_DATA
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST2A.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST2A
    jp  WAIT_MENU_CMD_RESPONSE_END  ; If time out waiting, return
WAIT_MENU_CMD_RESPONSE_ST2A.1:
    RECEIVE_DATA
    ld  b,a
WAIT_MENU_CMD_RESPONSE_ST2B:
    CHECK_DATA
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST2B.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_CMD_RESPONSE_ST2B
    jp  WAIT_MENU_CMD_RESPONSE_END  ; If time out waiting, return
WAIT_MENU_CMD_RESPONSE_ST2B.1:
    RECEIVE_DATA
    ld  c,a
    or  b                           ; Zero size in response?
    jr  z,WAIT_MENU_CMD_RESPONSE_END_OK
    ld  d,b
    ld  e,c                         ; Copy to DE
    ; Now loop getting the data until received everything or time-out
WAIT_MENU_CMD_RESPONSE_GET_DATA:
    CHECK_DATA
    jr  nz,WAIT_MENU_CMD_RESPONSE_GET_DATA.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_CMD_RESPONSE_GET_DATA
    jp  WAIT_MENU_CMD_RESPONSE_END  ; If time out waiting, return
WAIT_MENU_CMD_RESPONSE_GET_DATA.1:
    RECEIVE_DATA            ; Get data
    ld  (ix+0),a                    ; Put it in the buffer
    inc ix                          ; Increment pointer
    dec de                          ; Decrement counter
    ld  a,d
    or  e                           ; Is counter 0?
    jr  nz,WAIT_MENU_CMD_RESPONSE_GET_DATA
WAIT_MENU_CMD_RESPONSE_END_OK:
    ld  a,1
    or  a                           ; NZ to indicate success
    pop ix
    pop de
    ret
WAIT_MENU_CMD_RESPONSE_END:
    ld  b,#FF
    pop ix
    pop de
    ret
WAIT_MENU_CMD_RESPONSE_END_NOK:
    ld  hl,0
    ld  b,a                         ; Get result in B, 0xFF for time-out, otherwise was an error return code
    xor a
    pop ix
    pop de
    ret

; WAIT an ESP Wi-Fi Scan command Response
; Inputs:
; A -> Command Code
; HL -> Timeout
; IX -> Where to store response
;
; Returns:
; Flag Z is zero if failure, non zero if success
; A is the number of access points scanned
;
; Response is stored as:
; Access Point SSID zero terminated and after first 0
; 0 if Open otherwise requires a password to join
; And this repeats...
;
; Affect:
; AF , BC and HL
;
WAIT_MENU_SCMD_RESPONSE:
    push    de
    push    ix
    ld  d,a                         ; Command to wait in D
WAIT_MENU_SCMD_RESPONSE_ST1:
    LOAD_STS_PORT_IN_A
    bit 5,a                         ; If nz Port #07 is not available
    jp  nz,WAIT_MENU_SCMD_RESPONSE_END_NOK
    bit 0,a                         ; If nz has data
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST1.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST1
    jp  WAIT_MENU_SCMD_RESPONSE_END ; If time out waiting, return
WAIT_MENU_SCMD_RESPONSE_ST1.1:
    ; nz, check the data
    RECEIVE_DATA
    cp  d                           ; Is response of our command?
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST1
    ; Now get return code, if return code other than 0, it is finished
WAIT_MENU_SCMD_RESPONSE_RC:
    CHECK_DATA
    jr  nz,WAIT_MENU_SCMD_RESPONSE_RC.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_SCMD_RESPONSE_RC
    jp  WAIT_MENU_SCMD_RESPONSE_END ; If time out waiting, return
WAIT_MENU_SCMD_RESPONSE_RC.1:
    RECEIVE_DATA
    or  a                           ; 0?
    ; If not, done
    jp  nz,WAIT_MENU_SCMD_RESPONSE_END_NOK
    ; Next byte is how many access points are available
WAIT_MENU_SCMD_RESPONSE_ST2A:
    CHECK_DATA
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST2A.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST2A
    jp  WAIT_MENU_SCMD_RESPONSE_END ; If time out waiting, return
WAIT_MENU_SCMD_RESPONSE_ST2A.1:
    RECEIVE_DATA
    ld  b,a                         ; Save in B
    ld  c,a                         ; And C as well
    ; Now should loop this until c is 0, c will control access point received count
WAIT_MENU_SCMD_RESPONSE_ST2B:
    CHECK_DATA
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST2B.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST2B
    jp  WAIT_MENU_SCMD_RESPONSE_END ; If time out waiting, return
WAIT_MENU_SCMD_RESPONSE_ST2B.1:
    RECEIVE_DATA
    ld  (ix+0),a
    inc ix                          ; Increment pointer
    or  a                           ; Terminator of AP Name?
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST2B
    ; Get encryption
WAIT_MENU_SCMD_RESPONSE_GET_ENC:
    CHECK_DATA
    jr  nz,WAIT_MENU_SCMD_RESPONSE_GET_ENC.1
    call    HLTIMEOUT
    jr  nz,WAIT_MENU_SCMD_RESPONSE_GET_ENC
    jp  WAIT_MENU_SCMD_RESPONSE_END  ; If time out waiting, return
WAIT_MENU_SCMD_RESPONSE_GET_ENC.1:
    RECEIVE_DATA            ; Get data
    sub 'O'                         ; If O, open, will be 0, otherwise, will be notzero
    ld  (ix+0),a                    ; Put it in the buffer
    inc ix                          ; Increment pointer
    dec c                           ; Decrement counter
    xor a
    or  c                           ; Is counter 0?
    ; If not continue getting more SSIDs
    jr  nz,WAIT_MENU_SCMD_RESPONSE_ST2B
    ; It is zero, so, done
WAIT_MENU_SCMD_RESPONSE_END_OK:
    ld  a,1
    or  a                           ; NZ to indicate success
    ld  a,b                         ; Number of APs in A
WAIT_MENU_SCMD_RESPONSE_END:
    pop ix
    pop de
    ret
WAIT_MENU_SCMD_RESPONSE_END_NOK:
    ld  hl,0
    ld  b,a                         ; Return code in B
    xor a
    pop ix
    pop de
    ret

; This routine will check if HL is 0, if it is, will return immediatelly
; If it is not, will decrease HL value and halt (wait one interrupt)
HLTIMEOUT:
    ld  a,h
    or  l
    ret z
    dec hl
    halt
    ret

; Routine to print the string addressed by HL
PRINTHL:
    ld  a,(hl)
    or  a
    ret z                           ; When string is finished, done!
    call    CHPUT
    inc hl
    jp  PRINTHL

; Routine to print the string addressed by HL on a line, if exceeding it, ends with ..
PRINTHLINE:
    push    bc
    ld  b,24
PHLINEL:
    ld  a,(hl)
    or  a
    jr  z,PHLINESPC                 ; When string is finished, done!
    call    CHPUT
    inc hl
    djnz    PHLINEL
    ld  a,GOLEFT
    call    CHPUT
    call    CHPUT
    ld  a,1
    call    CHPUT
    ld  a,91
    call    CHPUT
    ld  a,' '
    call    CHPUT
    jr  PHLINELR
PHLINESPC:
    ld  a,' '
    call    CHPUT
    djnz    PHLINESPC
PHLINELR:
    ld  a,GOLEFT
    call    CHPUT
    call    CHPUT
    pop bc
    ret

;============================
;===  ESP helper routines  ===
;============================

WRFE_WAIT_DATA:
    LOAD_STS_PORT_IN_A
    bit 5,a                         ; If nz Port #07 is not available
    jr  nz,WRFE_STS_NOT_AVAILABLE
    bit 0,a                         ; if nz has data
    ret nz
    dec de
    halt
    ret
WRFE_STS_NOT_AVAILABLE:
    ld  de,0
    xor a
    ret

WRFE_COMPARE:
    ld  b,a
    ld  a,(hl)
    cp  b
    ret nz
    inc hl
    ret

WAIT_RESPONSE_FROM_ESP:
    ld  c,a                         ; Response size in C
    push    hl                      ; Save HL
    xor a
WRFE_ST1:
    ld  ixh,a                       ; We start at index 0

WRFE_LOOP:
    call    WRFE_WAIT_DATA
    jr  nz,WRFE_LOOP.1
    ld  a,e
    or  d
    jp  z,WRFE_RET_ERROR
    jr  WRFE_LOOP
WRFE_LOOP.1:
    ; nz, check the data
    RECEIVE_DATA
    ; Ok, now the byte is in A, let's compare
WRFE_IDXCMD:
    call WRFE_COMPARE
    ; if match
    jr  z,WRFE_RSP_MATCH
    ; did not match, let's zero the rsp index
    dec de
    ld  a,e
    or  d
    jr  z,WRFE_RET_ERROR
    xor a
    ld  ixh,a                       ; re-start at index 0
    pop hl                          ; restore the response index
    push    hl                      ; and keep it in stack
    ; back to get another byte
    jr  WRFE_LOOP
WRFE_RSP_MATCH:
    ; match
    inc ixh
    ld  a,ixh
    cp  c
    ; if a = c done and response is success
    jr  z,WRFE_RET_OK
    ; not done, back to get more bytes
    jr  WRFE_LOOP
WRFE_RET_OK:
    pop hl
    xor a
    ret
WRFE_RET_ERROR:
    pop hl
    ld  a,1
    ret

;*********************************************
;***              RESET ESP                ***
;*** If RESET ok, A will be 0, otherwise   ***
;*** failure                               ***
;*********************************************
RESET_ESP:
    ; Is ESP installed?
    CLEAR_UART                      ; Clear UART
    ld  a,'Q'
    call    CHPUT
    ld  a,CMD_QUERY_ESP
    SEND_DATA
    ld  a,'q'
    call    CHPUT
    ld  hl,RSP_CMD_QUERY_ESP        ; Expected response
    ld  de,60                       ; Up to 1s @ 60Hz
    ld  a,RSP_CMD_QUERY_ESP_SIZE    ; Size of response
    call    WAIT_RESPONSE_FROM_ESP
    or  a
    ld  a,1
    jr  z,RESET_ESP_INIT            ; ESP Found
    ld  a,'X'
    call    CHPUT
    call    SCAN_ESP_QUERY_SPEED
    ld  b,0                         ; 0 if no response
    ret
RESET_ESP_INIT:
    ld  a,'I'
    call    CHPUT
    ld  b,3                         ; Retry up to 10 times
RESET_ESP_LOOP:
    CLEAR_UART
    ld  a,'R'
    call    CHPUT
    SET_SPEED
    ld  a,'S'
    call    CHPUT
    halt
    halt                            ; Wait a little to make sure speed is adjusted
    ld  a,CMD_WRESET_ESP
    SEND_DATA
    ld  a,'W'
    call    CHPUT
    ld  hl,RSP_CMD_RESET_ESP        ; Expected response
    ld  de,90                       ; Up to 1.5s @ 60Hz
    ld  a,RSP_CMD_RESET_ESP_SIZE    ; Size of response
    push    bc                      ; Save retry counter
    call    WAIT_RESPONSE_FROM_ESP
    pop bc                          ; restore retry counter
    or  a                           ; Did WAIT RESPONSE return zero?
    jr  nz,RESET_ESP_LOOP_FAIL
    ld  a,'O'
    call    CHPUT
    xor a                           ; A=0, success (CHPUT clobbered flags so cannot rely on ret z)
    ret                             ; Warm Reset Ok
RESET_ESP_LOOP_FAIL:
    ld  a,'x'
    call    CHPUT
    djnz RESET_ESP_LOOP             ; No, decrement retry counter and let the loop check
    ; No more retries? Then check if it is old ESP FW
RESET_CHK_IF_INSTALLED:
    ; No more retries? Then it is old ESP FW
    ld  a,'F'
    call    CHPUT
    ld  b,1
    ret

SCAN_ESP_QUERY_SPEED:
    push    bc
    push    de
    push    hl
    ld  c,0
    ld  b,10
SCAN_ESP_QUERY_SPEED_LOOP:
    CLEAR_UART
    ld  a,c
    WRITE_CMD_PORT_A
    halt
    halt
    ld  a,CMD_QUERY_ESP
    SEND_DATA
    ld  hl,RSP_CMD_QUERY_ESP
    ld  de,20                       ; Short probe timeout per speed slot
    ld  a,RSP_CMD_QUERY_ESP_SIZE
    call    WAIT_RESPONSE_FROM_ESP
    or  a
    jr  z,SCAN_ESP_QUERY_SPEED_FOUND
    inc c
    djnz    SCAN_ESP_QUERY_SPEED_LOOP
    pop hl
    pop de
    pop bc
    ret
SCAN_ESP_QUERY_SPEED_FOUND:
    pop hl
    pop de
    pop bc
    ret

;*********************************************
;***              SET TIME                 ***
;*** H - Hour                              ***
;*** L - Minutes                           ***
;*** D - Seconds                           ***
;***                                       ***
;*** A - 0 if Ok otherwise invalid time    ***
;*********************************************
PATTERN_SETUP:
    xor a                           ; Bit-16, V9938 and V9958 only
    ld  hl,#0088                    ; Bit-15 to 0, pattern base address = #0088
    call SET_VDP_WRITE
    ld  hl,PATTERN_DATA             ; Point to hl
    ld  b,8*12                      ; Redraw 12 chars, max 12 chars are suggested
    ld  c,#98                       ; port #98
OUTI_TO_VRAM_TMS9918:
    outi                            ; Slower than otir but useful on TMS9918 which requires 29 T-states
    jp  nz,OUTI_TO_VRAM_TMS9918     ; Therefore, don't use 'djnz' here!
    ret
PATTERN_DATA:
; Pattern-01 is CHR$(1)+CHR$(81) = SCAN_TERMINATOR_OPEN
    db  %00000110
    db  %00001001
    db  %00001001
    db  %11111100                   ; "Opened Padlock"
    db  %11111100
    db  %11111100
    db  %11111100
    db  %00000000
; Pattern-02 is CHR$(1)+CHR$(82) = SCAN_TERMINATOR_ENC
    db  %00110000
    db  %01001000
    db  %01001000
    db  %11111100                   ; "Closed Padlock"
    db  %11111100
    db  %11111100
    db  %11111100
    db  %00000000
; Pattern-03 is CHR$(1)+CHR$(83) = Wi-Fi Connected to the AP:
    db  %00000000
    db  %00000000
    db  %01110000
    db  %01110000                   ; "Full Access Point"
    db  %01110000
    db  %00100000
    db  %00100000
    db  %01110000
; Pattern-04 is CHR$(1)+CHR$(84) = Wi-Fi Failed to connect to:
    db  %00000000
    db  %10001000
    db  %01010000
    db  %00100000                   ; "X"
    db  %01010000
    db  %10001000
    db  %00000000
    db  %00000000
; Pattern-05 is CHR$(1)+CHR$(85) = Requesting connection to:
    db  %00000000
    db  %00000000
    db  %01010000
    db  %00100000                   ; "Partial Access Point"
    db  %01010000
    db  %00100000
    db  %00100000
    db  %01110000
; Pattern-06 is CHR$(1)+CHR$(86) = Wi-Fi Did not find the AP:
    db  %00000000
    db  %01110000
    db  %10001000
    db  %00010000                   ; "?"
    db  %00100000
    db  %00000000
    db  %00100000
    db  %00000000
; Pattern-07 is CHR$(1)+CHR$(87) = Wi-Fi is Idle, AP configured:
    db  %00000000
    db  %00000000
    db  %00000000
    db  %00100000                   ; "i"
    db  %00000000
    db  %00100000
    db  %00100000
    db  %01110000
; Pattern-08 is CHR$(1)+CHR$(88) = Left Middle Wave
    db  %00000000
    db  %00100000
    db  %01000000
    db  %01000000                   ; "("
    db  %01000000
    db  %00100000
    db  %00000000
    db  %00000000
; Pattern-09 is CHR$(1)+CHR$(89) = Right Middle Wave
    db  %00000000
    db  %00100000
    db  %00010000
    db  %00010000                   ; ")"
    db  %00010000
    db  %00100000
    db  %00000000
    db  %00000000
; Pattern-10 is CHR$(1)+CHR$(90) = Wi-Fi is reconnecting to:
    db  %00000000
    db  %00000000
    db  %00000000
    db  %10101000                   ; "..."
    db  %00000000
    db  %00000000
    db  %00000000
    db  %00000000
; Pattern-11 is CHR$(1)+CHR$(91) = Double Dot
    db  %00000000
    db  %00000000
    db  %00000000
    db  %00000000                   ; ".."
    db  %00000000
    db  %01100110
    db  %01100110
    db  %00000000
; Pattern-12 is CHR$(1)+CHR$(92) = Copyright
    db  %00111000
    db  %01000100
    db  %10111010
    db  %10100010                   ; "(c)"
    db  %10111010
    db  %01000100
    db  %00111000
    db  %00000000
; http://map.grauw.nl/articles/vdp_tut.php
; Set VDP address counter to write from address AHL (17-bits)
; Enables the interrupts
SET_VDP_WRITE:
    rlc h
    rla
    rlc h
    rla
    srl h
    srl h
    di
    out (#99),a
    ld  a,14+128
    out (#99),a
    ld  a,l
    nop                             ; Do not remove this 'nop' here!
    out (#99),a
    ld  a,h
    or  64
    ei
    out (#99),a
    ret

;*********************************************
;***    ESP Specific Commands/Responses    ***
;*********************************************
; Cold reset of ESP firmware
CMD_RESET_ESP           equ 'R'
; Warm reset of ESP firmware
CMD_WRESET_ESP          equ 'W'
; Hold Wi-Fi Connection On
CMD_WIFIHOLD_ESP        equ 'H'
; Release Wi-Fi Connection Hold
CMD_WIFIRELEASE_ESP     equ 'h'
; Query Auto Clock settings
CMD_QUERY_ACLK_SETTINGS equ 'c'
; Set Auto Clock settings
CMD_SET_ACLK_SETTINGS   equ 'C'
; Query ESP settings
CMD_QUERY_ESP_SETTINGS  equ 'Q'
; Set Timer Value
CMD_TIMER_SET           equ 'T'
; Turn Nagle On
CMD_NAGLE_ON            equ 'D'
; Turn Nagle Off
CMD_NAGLE_OFF           equ 'N'
; Request to connect to a network
CMD_WIFI_CONNECT        equ 'A'
; Request to start network scan
CMD_SCAN_START          equ 'S'
; Request network scan result
CMD_SCAN_RESULTS        equ 's'
; Request AP Status
CMD_AP_STS              equ 'g'
; After finishing Warm reset, ESP returns ready
RSP_CMD_RESET_ESP       db  "Ready"
RSP_CMD_RESET_ESP_SIZE  equ 5
; Query ESP Presence
CMD_QUERY_ESP           equ '?'
; Query response
RSP_CMD_QUERY_ESP       db  "OK"
RSP_CMD_QUERY_ESP_SIZE  equ 2

;--- Strings
; Special thanks to KdL
; He has contributed a lot to make the menus and strings
; concise and much easier to read and understand!
STTERMINATOR            equ 0
LF                      equ 10
HOME                    equ 11
CLS                     equ 12
CR                      equ 13
GORIGHT                 equ 28
GOLEFT                  equ 29

ENTERING_WIFI_SETUP:
    db  CLS
    db  "Entering Wi-Fi Setup..."       ,CR,LF,LF,STTERMINATOR
;---

WELCOME:
    db  CLS
    db  "PICOVERSE Wi-Fi Setup 1.3"       ,CR,LF
    db  1,92," 2024 Oduvaldo Pavan Junior" ,CR,LF
    db  "ducasp@gmail.com"              ,CR,LF,LF,STTERMINATOR
;---

WELCOME_SF:
    db  "Quick Receive supported."      ,CR,LF,LF,STTERMINATOR
;---

WELCOME_SF2:
    db  "Quick Receive not supported."      ,CR,LF,LF,STTERMINATOR
;---

WELCOME_CS:
    db  "Wi-Fi is reconnecting to:    " ,GOLEFT,CR,LF
    db  "(",1,88,1,90,1,89,") "         ,STTERMINATOR

WELCOME_NEXT:
    db  HOME
    db  LF
    db  LF
    db  LF,LF,STTERMINATOR
;---

WELCOME_S_NEXT:
    db  LF
    db  LF,LF,STTERMINATOR
;---

WELCOME_SF_NEXT:
    db  LF,LF,STTERMINATOR
;---

WELCOME_CS0_NEXT:
    db  "Wi-Fi is Idle, AP configured:" ,GOLEFT,CR,LF
    db  "(",1,88,1,87,1,89,") "         ,STTERMINATOR

WELCOME_CS1_NEXT:
    db  CR,LF
    db  GORIGHT,GORIGHT,GORIGHT,GORIGHT,GORIGHT,GORIGHT,STTERMINATOR

WELCOME_CS2_NEXT:
    db  "Wi-Fi Wrong Password for:    " ,GOLEFT,CR,LF
    db  "(",1,88,1,82,1,89,") "         ,STTERMINATOR

WELCOME_CS3_NEXT:
    db  "Wi-Fi Did not find the AP:   " ,GOLEFT,CR,LF
    db  "(",1,88,1,86,1,89,") "         ,STTERMINATOR

WELCOME_CS4_NEXT:
    db  "Wi-Fi Failed to connect to:  " ,GOLEFT,CR,LF
    db  "(",1,88,1,84,1,89,") "         ,STTERMINATOR

WELCOME_CS5_NEXT:
    db  "Wi-Fi Connected to the AP:   " ,GOLEFT,CR,LF
    db  "(",1,88,1,83,1,89,") "         ,STTERMINATOR

MMENU_S_NEXT:
    db  CR,LF,LF
    db  "1 - Set Nagle Algorithm"       ,CR,LF
    db  "2 - Set Wi-Fi On Period"       ,CR,LF
    db  "3 - Scan/Join Access Points"   ,CR,LF
    db  "4 - Wi-Fi and Clock Settings"  ,CR,LF,LF
;---
    db  "ESC to exit setup."            ,CR,LF,LF
;---
    db  "Option: "                      ,STTERMINATOR

MMENU_CLOCK_MSX2:
    db  CLS
    db  "[ Wi-Fi and Clock Settings ]"  ,CR,LF,LF
;---
    db  "0 - Wi-Fi adapter online"      ,CR,LF
    db  "1 - Also wait up to 10s for"   ,CR,LF
    db  "    internet availability and" ,GOLEFT,CR,LF
    db  "    get time from SNTP server" ,GOLEFT,CR,LF
    db  "    adjusting the time zone"   ,CR,LF
    db  "2 - The same as option 1 but"  ,CR,LF
    db  "    also will turn off Wi-Fi"  ,CR,LF
    db  "    when done"                 ,CR,LF
    db  "3 - Wi-Fi adapter offline"     ,GOLEFT,CR,LF,LF
;---
    db  "MSX boot will take longer if"  ,CR,LF
    db  "options 1 or 2 are active."    ,CR,LF,LF,STTERMINATOR
;---

MMENU_CLOCK_MSX1:
    db  CLS
    db  "[ Wi-Fi and Clock Settings ]"  ,CR,LF,LF
;---
    db  "0 - Wi-Fi adapter online"      ,CR,LF
    db  "1 - Unavailable for MSX1"      ,CR,LF
    db  "2 - Unavailable for MSX1"      ,CR,LF
    db  "3 - Wi-Fi adapter offline"     ,GOLEFT,CR,LF,LF,STTERMINATOR
;---

MMENU_CLOCK_0:
    db  "Currently: ONLINE"             ,STTERMINATOR

MMENU_CLOCK_1:
    db  "Currently: TIME-OPT1, GMT"     ,STTERMINATOR

MMENU_CLOCK_2:
    db  "Currently: TIME-OPT2, GMT"     ,STTERMINATOR

MMENU_CLOCK_3:
    db  "Currently: OFFLINE"            ,STTERMINATOR

MMENU_CLOCK_OPT:
    db  CR,LF,LF
;---
    db  "ESC to return to main menu."   ,CR,LF,LF
;---
    db  "Option: "                      ,STTERMINATOR

MMENU_GMT_OPT:
    db  CR,LF
    db  "Time Zone adjustment: "        ,STTERMINATOR

MMENU_MANUALENTRY:
    db  CLS
    db  "[ Scan/Join Access Points ]"   ,CR,LF,LF
;---
    db  "ESC to return to main menu."   ,CR,LF,LF
;---
    db  "Manual entry, type SSID:"      ,CR,LF
    db  "(",1,88,1,83,1,89,") "         ,STTERMINATOR

MENU_MANUALENTRY_PWD:
    db  CR,LF,LF
;---
    db  "Password needed (y/n)? "             ,STTERMINATOR

MMENU_SCAN:
    db  CLS
    db  "[ Scan/Join Access Points ]"   ,CR,LF,LF
;---
    db  "Up to ",SCAN_MAX_PAGE_SIZE+48," APs per page." ,CR,LF,LF
;---
    db  "Scanning networks..."          ,CR,LF,STTERMINATOR

MMENU_SCANF:
    db  CR,LF
    db  "Error or no networks found!"   ,CR,LF,STTERMINATOR

MMENU_SCANN:
    db  CR,LF
    db  "No networks found!"            ,CR,LF,STTERMINATOR

MMENU_SCANS:
    db  CLS
    db  "[ Scan/Join Access Points ]"   ,CR,LF,LF
;---
    db  "Networks available:"           ,CR,LF,LF,STTERMINATOR
;---

MMENU_CONNECTING:
    db  CLS
    db  "[ Scan/Join Access Points ]"   ,CR,LF,LF
;---
    db  "Requesting connection to:"     ,CR,LF
    db  "(",1,88,1,85,1,89,") "         ,STTERMINATOR

MMENU_ASKPWD:
    db  CR,LF
    db  "Hit DEL as first character"    ,CR,LF
    db  "to hide/show the typing."      ,CR,LF
    db  "Password: "                    ,STTERMINATOR

MMENU_SCANQ:
    db  CR,LF
    db  "ESC to return to main menu."   ,CR,LF
    db  "SPACE BAR to scan again."  ,CR,LF
    db  "ENTER to type SSID/AP name."   ,CR,LF,LF
;---
    db  "Number to connect: "       ,STTERMINATOR

MMENU_SCANQM:
    db  CR,LF
    db  "ESC to return to main menu."   ,CR,LF
    db  "SPACE BAR to show next page."  ,CR,LF
    db  "ENTER to type SSID/AP name."   ,CR,LF,LF
;---
    db  "Number to connect: "       ,STTERMINATOR

SCAN_TERMINATOR_CUT:
    db  GOLEFT,GOLEFT,1,91," ",GOLEFT,STTERMINATOR

SCAN_TERMINATOR_OPEN:
    db  " ",1,81,GOLEFT,CR,LF,STTERMINATOR

SCAN_TERMINATOR_ENC:
    db  " ",1,82,GOLEFT,CR,LF,STTERMINATOR

MMENU_TIMEOUT:
    db  CLS
    db  "   [ Set Wi-Fi On Period ]"    ,CR,LF,LF
;---
    db  "Wi-Fi On Period allows to set" ,GOLEFT,CR,LF
    db  "a given period of time of"     ,CR,LF
    db  "inactivity to turn off Wi-Fi"  ,CR,LF
    db  "automatically."                ,CR,LF,LF
;---
    db  "0         - Always on"         ,CR,LF
    db  "1 to 30   - 30s"               ,CR,LF
    db  "31 to 600 - Use given period"  ,CR,LF
    db  "> 600     - 600s"              ,CR,LF,LF,STTERMINATOR
;---

MMENU_TIMEOUT_ALWAYSON:
    db  "Wi-Fi is currently: ALWAYS ON" ,GOLEFT,CR,LF,LF
;---
    db  "ESC to return to main menu."   ,CR,LF,LF
;---
    db  "Type desired period: "         ,STTERMINATOR

MMENU_TIMEOUT_NOTALWAYSON1:
    db  "Wi-Fi period set to: "         ,STTERMINATOR

MMENU_TIMEOUT_NOTALWAYSON2:
    db  "s"                             ,CR,LF,LF
;---
    db  "ESC to return to main menu."   ,CR,LF,LF
;---
    db  "Type desired period: "         ,STTERMINATOR

MMENU_NAGLE:
    db  CLS
    db  "   [ Set Nagle Algorithm ]"    ,CR,LF,LF
;---
    db  "Nagle Algorithm might lower"   ,CR,LF
    db  "performance but create less"   ,CR,LF
    db  "network congestion."           ,CR,LF
    db  "Nowadays it is mostly not"     ,CR,LF
    db  "needed and is the cause of"    ,CR,LF
    db  "latency and low performance"   ,CR,LF
    db  "on packet-driven protocols."   ,CR,LF,LF
;---
    db  "O - Turn it on/off"            ,CR,LF,LF,STTERMINATOR
;---

MMENU_NAGLE_ON:
    db  "Nagle is currently: ON"        ,CR,LF,LF
;---
    db  "ESC to return to main menu."   ,CR,LF,LF
;---
    db  "Option: "                      ,STTERMINATOR

MMENU_NAGLE_OFF:
    db  "Nagle is currently: OFF"       ,CR,LF,LF
;---
    db  "ESC to return to main menu."   ,CR,LF,LF
;---
    db  "Option: "                      ,STTERMINATOR

STR_SENDING:
    db  "Sending command, wait..."      ,CR,LF,STTERMINATOR

STR_SENDING_OK:
    db  "Command sent Ok, done!"        ,CR,LF,STTERMINATOR

STR_SENDING_OK_JN:
    db  CR,GORIGHT,GORIGHT,1,83,CR,LF,LF
;---
    db  "Successfully connected!"       ,CR,LF,STTERMINATOR

STR_SENDING_NOK_JN:
    db  CR,GORIGHT,GORIGHT,1,84,CR,LF,LF
;---
    db  "Fail to connect, if protected" ,GOLEFT,CR,LF
    db  "network check password!"       ,CR,LF,STTERMINATOR

STR_SENDING_FAIL:
    db  "Command failure..."            ,CR,LF,STTERMINATOR

FAIL_S:
    db  CLS
    db  "ESP8266 Not Found!"            ,CR,LF,LF
;---
    db  "Check that it is properly"     ,CR,LF
    db  "inserted in its connector."    ,CR,LF,STTERMINATOR

FAIL_F:
    db  CLS
    db  "ESP8266 FW Update Required!"   ,CR,LF,STTERMINATOR

SEG_CODE_END:
; End of configuration-only ROM payload