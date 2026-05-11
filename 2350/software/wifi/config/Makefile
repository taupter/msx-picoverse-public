######################################################################
# ESP8266 Wi-Fi configuration ROM build
######################################################################

SJASM := ../utils/sjasm.exe
ASM := ESP8266_config.asm
ROM := ESP8266_config.rom
LST := ESP8266_config_build.lst
SYM := ESP8266_config_build.sym
GEN_ROM := ESP8266_config.out
GEN_LST := ESP8266_config.lst
GEN_SYM := ESP8266_config.sym
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
	@echo "Cleaning WiFi config ROM build artifacts"
	@$(RM) "$(ROM)" "$(LST)" "$(SYM)" "$(GEN_ROM)" "$(GEN_LST)" "$(GEN_SYM)"
