T := $(OO_PS4_TOOLCHAIN)
CC := $T/bin/linux/clang
CF := --target=x86_64-scei-ps4-elf -O2 -I$T/include -Iexternal/cJSON
LIBS := -lSDL2 -lSDL2_image -lSDL2_ttf -lSceNet -lSceHttp -lSceSsl -lSceAppInstUtil \
        -lSceSysmodule -lScePad -lSceImeDialog -lSceKernel -lpthread -lm -lc
SRC := faststore.c external/cJSON/cJSON.c
all: eboot.bin
eboot.bin: $(SRC)
	$(CC) $(CF) $(SRC) -o eboot.bin -L$T/lib $(LIBS)
	$T/bin/linux/create-fself -in=eboot.bin -out=eboot.bin --paid 0x3800000000000011
