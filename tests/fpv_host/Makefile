FPV := ../../main/fpv
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -I$(FPV)

host_dsp: host_dsp.c $(FPV)/dsp.c $(FPV)/analysis.c $(FPV)/channels.c $(FPV)/link.c $(FPV)/link.h $(FPV)/dsp.h $(FPV)/analysis.h
	$(CC) $(CFLAGS) -o $@ host_dsp.c $(FPV)/dsp.c $(FPV)/analysis.c $(FPV)/channels.c $(FPV)/link.c -lm

# The first (64-bit) FFT, kept for comparison.
host_dsp_ref: host_dsp.c $(FPV)/dsp.c $(FPV)/analysis.c $(FPV)/channels.c $(FPV)/link.c $(FPV)/dsp.h $(FPV)/analysis.h
	$(CC) $(CFLAGS) -DFPV_FFT_REF -o $@ host_dsp.c $(FPV)/dsp.c $(FPV)/analysis.c $(FPV)/channels.c $(FPV)/link.c -lm

clean:
	rm -rf host_dsp host_dsp_ref host_ui obj iq.bin sweep.bin *.ppm

# Screen previews: LovyanGFX in memory (Linux framebuffer platform, no device).
LGFX := ../../components/LovyanGFX/src
LGFX_SRCS := $(LGFX)/lgfx/v1/lgfx_v1.cpp $(LGFX)/lgfx/v1/LGFXBase.cpp $(LGFX)/lgfx/v1/lgfx_fonts.cpp \
             $(LGFX)/lgfx/v1/lgfx_v1_panel.cpp $(LGFX)/lgfx/v1/lgfx_v1_platforms.cpp \
             $(LGFX)/lgfx/utility/lgfx_miniz.c $(LGFX)/lgfx/utility/lgfx_pngle.c \
             $(LGFX)/lgfx/utility/lgfx_qoi.c $(LGFX)/lgfx/utility/lgfx_qrcode.c $(LGFX)/lgfx/utility/lgfx_tjpgd.c \
             $(wildcard $(LGFX)/lgfx/Fonts/efont/*.c) $(LGFX)/lgfx/Fonts/IPA/lgfx_font_japan.c
LGFX_OBJS := $(patsubst $(LGFX)/%,obj/%.o,$(LGFX_SRCS))

obj/%.o: $(LGFX)/%
	@mkdir -p $(dir $@)
	$(CXX) -x $(if $(filter %.c,$<),c,c++) -O1 -w -DLGFX_LINUX_FB -I$(LGFX) -c $< -o $@

host_ui: host_ui.cpp $(FPV)/ui.cpp $(FPV)/ui.hpp $(FPV)/dsp.c $(FPV)/analysis.c $(FPV)/channels.c $(LGFX_OBJS)
	$(CXX) -O1 -std=gnu++17 -DLGFX_LINUX_FB -I$(LGFX) -I$(FPV) -o $@ host_ui.cpp $(FPV)/ui.cpp \
	  -x c $(FPV)/dsp.c $(FPV)/analysis.c $(FPV)/channels.c $(FPV)/link.c -x none $(LGFX_OBJS) -lm -lpthread
