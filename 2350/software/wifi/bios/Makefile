######################################################################
# Vendored ESP8266P WiFi BIOS build
######################################################################

SJASM := ../utils/sjasm.exe
ASM := ESP8266_memio.asm
ROM := ESP8266P.rom
LST := ESP8266P.lst
SYM := ESP8266P.sym
GEN_ROM := ESP8266_memio.out
GEN_LST := ESP8266_memio.lst
GEN_SYM := ESP8266_memio.sym
RM := rm -f

.PHONY: all clean

all: $(ROM)

$(ROM): $(ASM) $(SJASM)
	@echo "Assembling $(ROM)"
	@"$(SJASM)" "$(ASM)"
	@if [ -f "$(GEN_ROM)" ]; then mv "$(GEN_ROM)" "$(ROM)"; fi
	@if [ -f "$(GEN_LST)" ]; then mv "$(GEN_LST)" "$(LST)"; fi
	@if [ -f "$(GEN_SYM)" ]; then mv "$(GEN_SYM)" "$(SYM)"; fi
	@if [ ! -f "$(ROM)" ]; then echo "Missing $(ROM) after assembly"; exit 1; fi

clean:
	@echo "Cleaning WiFi BIOS build artifacts"
	@$(RM) "$(ROM)" "$(LST)" "$(GEN_ROM)" "$(GEN_LST)" "$(GEN_SYM)"