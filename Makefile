SOURCES=common.c cpu.c cpu-addressing.c fce.c main.c memory.c mmc.c ppu.c psg.c
TARGET_BINARY=mynes
# LIBS=-lallegro-static -lallegro_main-static -lallegro_primitives-static -framework AppKit -framework OpenGL /usr/lib/libIOKit.dylib -framework AGL
LIBS=-lallegro -lallegro_main -lallegro_primitives
FLAGS=

all:
	clang $(FLAGS) $(LIBS) $(SOURCES) -o $(TARGET_BINARY)

optrace:
	clang $(FLAGS) -DOP_TRACE=1 $(SOURCES) $(LIBS) -o $(TARGET_BINARY)

cputest:
	clang $(FLAGS) -DCPU_TEST $(LIBS) $(SOURCES) -o $(TARGET_BINARY) && ./$(TARGET_BINARY) tests/nestress.nes

pputest:
	clang $(FLAGS) -DPPU_TEST $(LIBS) $(SOURCES) -o $(TARGET_BINARY) && ./$(TARGET_BINARY) tests/nestress.nes

vram_accesstest:
	clang $(FLAGS) -DVRAMACC_TEST $(LIBS) $(SOURCES) -o $(TARGET_BINARY) && ./$(TARGET_BINARY) tests/vram_access.nes
