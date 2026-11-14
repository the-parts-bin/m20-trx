# Tomb Raider (TRX at its first standalone, fully decompiled build) for the
# m20 SoC: the platform layer in src/specific/ (software renderer, mixer,
# keys) over src/m20/m20_platform.h.
#
# make (make board): the m20 build, build-WxH/tomb1.bin, which the core
# firmware's loader copies into SDRAM and runs; needs the m20 core (CORE:
# ../core in m20-system, else ../m20-system/core), riscv64-unknown-elf-gcc and
# picolibc-riscv64-unknown-elf. The game's files go in TOMB1/ on the card
# (DATA/ and cfg/, as for the harness).
#
# make host: the test harness, 32-bit x86 Linux with SDL2 (libsdl2-dev:i386),
# with the board's frame size and heap: build-host-WxH/tomb1 (RES=640x360 by
# default; RES=320x240). Run it from a directory holding cfg/ and DATA/.

all: board

RES ?= 640x360
RES_W := $(word 1,$(subst x, ,$(RES)))
RES_H := $(word 2,$(subst x, ,$(RES)))

# The board's memory: 8 MB of SDRAM less the image, data, bss and the
# 128 KB stack. The harness's heap (M20_HEAP) is the same: the board
# build's (from __heap_start in its ELF, when it has been built; else
# 4.75 MB). The game's buffer (M20_GAMEBUF, the levels' data) comes from it.
BOARD_ELF    = $(B)/tomb1.elf
M20_HEAP    ?= $(if $(wildcard $(BOARD_ELF)),$(shell printf '0x%x' $$(( 0xFE0000 - 0x$$(riscv64-unknown-elf-nm $(BOARD_ELF) | awk '$$3 == "__heap_start" { print $$1 }') ))),0x4C0000)
M20_GAMEBUF ?= 0x420000

SRC := src/3dsystem/3d_gen.c \
       src/3dsystem/phd_math.c \
       src/3dsystem/scalespr.c \
       src/args.c \
       src/config.c \
       src/filesystem.c \
       src/game/ai/abortion.c \
       src/game/ai/alligator.c \
       src/game/ai/ape.c \
       src/game/ai/baldy.c \
       src/game/ai/bat.c \
       src/game/ai/bear.c \
       src/game/ai/centaur.c \
       src/game/ai/cowboy.c \
       src/game/ai/crocodile.c \
       src/game/ai/dino.c \
       src/game/ai/evil_lara.c \
       src/game/ai/larson.c \
       src/game/ai/lion.c \
       src/game/ai/mummy.c \
       src/game/ai/mutant.c \
       src/game/ai/natla.c \
       src/game/ai/pierre.c \
       src/game/ai/pod.c \
       src/game/ai/raptor.c \
       src/game/ai/rat.c \
       src/game/ai/skate_kid.c \
       src/game/ai/statue.c \
       src/game/ai/vole.c \
       src/game/ai/wolf.c \
       src/game/box.c \
       src/game/camera.c \
       src/game/cinema.c \
       src/game/clock.c \
       src/game/collide.c \
       src/game/control.c \
       src/game/demo.c \
       src/game/draw.c \
       src/game/effects/blood.c \
       src/game/effects/body_part.c \
       src/game/effects/bubble.c \
       src/game/effects/chain_block.c \
       src/game/effects/dino_stomp.c \
       src/game/effects/earthquake.c \
       src/game/effects/explosion.c \
       src/game/effects/finish_level.c \
       src/game/effects/flicker.c \
       src/game/effects/flipmap.c \
       src/game/effects/flood.c \
       src/game/effects/lara_effects.c \
       src/game/effects/missile.c \
       src/game/effects/powerup.c \
       src/game/effects/raising_block.c \
       src/game/effects/ricochet.c \
       src/game/effects/sand.c \
       src/game/effects/splash.c \
       src/game/effects/stairs2slope.c \
       src/game/effects/turn_180.c \
       src/game/effects/twinkle.c \
       src/game/effects/waterfall.c \
       src/game/fmv.c \
       src/game/game.c \
       src/game/gamebuf.c \
       src/game/gameflow.c \
       src/game/hair.c \
       src/game/input.c \
       src/game/inventry.c \
       src/game/invfunc.c \
       src/game/invvars.c \
       src/game/items.c \
       src/game/lara.c \
       src/game/lara1gun.c \
       src/game/lara2gun.c \
       src/game/larafire.c \
       src/game/laramisc.c \
       src/game/larasurf.c \
       src/game/laraswim.c \
       src/game/level.c \
       src/game/lot.c \
       src/game/music.c \
       src/game/objects/boat.c \
       src/game/objects/bridge.c \
       src/game/objects/cabin.c \
       src/game/objects/cog.c \
       src/game/objects/door.c \
       src/game/objects/gunshot.c \
       src/game/objects/keyhole.c \
       src/game/objects/misc.c \
       src/game/objects/pickup.c \
       src/game/objects/puzzle_hole.c \
       src/game/objects/savegame_crystal.c \
       src/game/objects/scion.c \
       src/game/objects/switch.c \
       src/game/objects/trapdoor.c \
       src/game/option.c \
       src/game/overlay.c \
       src/game/pause.c \
       src/game/people.c \
       src/game/picture.c \
       src/game/random.c \
       src/game/requester.c \
       src/game/savegame.c \
       src/game/screen.c \
       src/game/settings.c \
       src/game/setup.c \
       src/game/shell.c \
       src/game/sound.c \
       src/game/sphere.c \
       src/game/text.c \
       src/game/traps/damocles_sword.c \
       src/game/traps/dart.c \
       src/game/traps/dart_emitter.c \
       src/game/traps/falling_block.c \
       src/game/traps/falling_ceiling.c \
       src/game/traps/flame.c \
       src/game/traps/lava.c \
       src/game/traps/lightning_emitter.c \
       src/game/traps/midas_touch.c \
       src/game/traps/movable_block.c \
       src/game/traps/pendulum.c \
       src/game/traps/rolling_ball.c \
       src/game/traps/rolling_block.c \
       src/game/traps/spikes.c \
       src/game/traps/teeth_trap.c \
       src/game/traps/thors_hammer.c \
       src/game/viewport.c \
       src/global/vars.c \
       src/global/vars_platform.c \
       src/init.c \
       src/json.c \
       src/log.c \
       src/memory.c \
       src/specific/s_clock.c \
       src/specific/s_display.c \
       src/specific/s_filesystem.c \
       src/specific/s_fmv.c \
       src/specific/s_frontend.c \
       src/specific/s_hwr.c \
       src/specific/s_input.c \
       src/specific/s_music.c \
       src/specific/s_output.c \
       src/specific/s_picture_pcx.c \
       src/specific/s_shell.c \
       src/specific/s_sound.c

# -------------------------------------------------------------------------
# The board
# -------------------------------------------------------------------------
CORE    ?= $(firstword $(wildcard ../core ../m20-system/core) ../core)
# The engine's features, all in core's current bitstream (each off with
# NAME=): ZENGINE its depth test (the opaque faces streamed, unsorted);
# TILED the pages in 8 x 8 tiles (its texel cache); EDGES its edge walk and
# palette banks (the faces' rows and those under water)
ZENGINE ?= 1
TILED   ?= 1
EDGES   ?= 1
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime
CROSS   ?= riscv64-unknown-elf-
B       := build-$(RES)$(if $(PROFILE),-prof)$(if $(TIMEDEMO),-td)$(if $(ZENGINE),-z)$(if $(SPANOFF),-cpu)$(if $(TILED),-t)$(if $(TIMERS),-tm)$(if $(EDGES),-e)
ARCH    := -march=rv32imf_zba_zicbop_zicsr_zifencei_zicbom_zicboz -mabi=ilp32f
CFLAGS_BOARD := $(ARCH) -O2 -g --picolibc-buildtype=release -specs=picolibc.specs -std=gnu11 \
                -ffunction-sections -fdata-sections -fno-strict-aliasing \
                -DM20 -DM20_BOARD -DM20_W=$(RES_W) -DM20_H=$(RES_H) -DM20_GAMEBUF=$(M20_GAMEBUF) \
                -DVERSION='"T1M m20"' -DFF_FS_READONLY=0 -Isrc -I$(CORE_FW) -I$(CORE_FW)/fatfs \
                -Wall -Wno-unused -Wno-sign-compare -Wno-missing-braces -Wno-parentheses \
                -Wno-pointer-sign -Wno-unused-result -Wno-format -Wno-stringop-truncation -Wno-maybe-uninitialized \
                -fsingle-precision-constant -Wdouble-promotion -DPICOLIBC_INTEGER_PRINTF_SCANF \
                $(if $(PROFILE),-DM9B_PROFILE -DPROF_TOP=0) $(if $(TIMEDEMO),-DM20_TIMEDEMO) $(if $(ZENGINE),-DM20_ZENGINE) $(if $(SPANOFF),-DM20_SPANOFF) $(if $(TILED),-DM20_TILED) $(if $(TIMERS),-DM20_TIMERS) $(if $(EDGES),-DM20_EDGES)
# make TIMEDEMO=1: one game step per frame, the demos timed (build-WxH-td-.../)
# make TIMERS=1: cycle timers of the frame's stages, on the fps line
# make SPANOFF=1: the CPU draws everything (-cpu)
# make PROFILE=1: the PC-sampling profiler (build-WxH-prof/; Ctrl-P on the
# UART prints it, core's tools/m9bctl.py prof and tools/prof.py read it)
LDFLAGS_BOARD := $(ARCH) -specs=picolibc.specs --picolibc-buildtype=release -DPICOLIBC_INTEGER_PRINTF_SCANF \
                 -Wl,--defsym=__stack_size=0x20000 -Wl,--gc-sections -Wl,-Map=$(B)/tomb1.map \
                 -T$(CORE_RT)/m9b.ld
CORE_SRC := $(CORE_RT)/syscalls.c $(CORE_RT)/string.c $(CORE_RT)/trap.c $(CORE_RT)/shot.c \
            $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/flashdisk.c $(CORE_FW)/fatfs/ff.c \
            $(CORE_FW)/fatfs/ffunicode.c
BOBJ := $(patsubst src/%.c,$(B)/%.o,$(SRC) src/m20/board.c) \
        $(patsubst %.c,$(B)/core/%.o,$(notdir $(CORE_SRC))) $(B)/core/trap_entry.o

board: $(B)/tomb1.bin

$(B)/%.o: src/%.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(CFLAGS_BOARD) -MMD -MP -c $< -o $@

vpath %.c $(CORE_RT) $(CORE_FW) $(CORE_FW)/fatfs
vpath %.S $(CORE_RT)

$(B)/core/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(CFLAGS_BOARD) -MMD -MP -c $< -o $@

# The string functions must not be turned back into calls to themselves.
$(B)/core/string.o: CFLAGS_BOARD += -fno-tree-loop-distribute-patterns -fno-builtin

$(B)/core/%.o: %.S
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(CFLAGS_BOARD) -c $< -o $@

$(B)/tomb1.elf: $(BOBJ) $(CORE_RT)/m9b.ld
	$(CROSS)gcc $(LDFLAGS_BOARD) $(BOBJ) -lm -o $@
	$(CROSS)size $@

$(B)/tomb1.bin: $(B)/tomb1.elf
	$(CROSS)objcopy -O binary $< $@
	@ls -l $@

-include $(BOBJ:.o=.d)

# -------------------------------------------------------------------------
# The host harness
# -------------------------------------------------------------------------
HB := build-host-$(RES)$(if $(TILED),-t)$(if $(EDGES),-e)
CFLAGS_HOST := -m32 -msse2 -mfpmath=sse -O2 -g -std=gnu11 -D_GNU_SOURCE -DM20 -DM20_W=$(RES_W) -DM20_H=$(RES_H) $(if $(TILED),-DM20_TILED) $(if $(EDGES),-DM20_EDGES) \
               -DM20_HEAP=$(M20_HEAP)u -DM20_GAMEBUF=$(M20_GAMEBUF) -DVERSION='"T1M m20"' \
               -Isrc -I$(CORE_FW) -Wall -Wno-unused -Wno-sign-compare -Wno-missing-braces -Wno-parentheses \
               -Wno-pointer-sign -Wno-unused-result -Wno-format-truncation \
               $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags sdl2)
LIBS_HOST := $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2) -lm
WRAP := -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup

HOBJ := $(patsubst src/%.c,$(HB)/%.o,$(SRC) src/m20/host_sdl.c)

host: $(HB)/tomb1

$(HB)/%.o: src/%.c Makefile
	@mkdir -p $(dir $@)
	gcc $(CFLAGS_HOST) -MMD -MP -c $< -o $@

$(HB)/tomb1: $(HOBJ)
	@echo "heap: $(M20_HEAP)"
	gcc -m32 $(HOBJ) $(WRAP) $(LIBS_HOST) -o $@

-include $(HOBJ:.o=.d)

clean:
	rm -rf build-*

.PHONY: all board host clean
