CXX = g++
CLANG = clang
BPFTOOL = bpftool

CXXFLAGS = -O2 -Wall -std=c++17 -I../../../include

# ── Arrow / Parquet support ───────────────────────────────────────────────────
# Enabled by default when the system libraries are present.  Override with:
#   make USE_PARQUET=0   to build without Parquet support
# ─────────────────────────────────────────────────────────────────────────────
ifndef USE_PARQUET
    ifeq ($(shell pkg-config --exists arrow parquet 2>/dev/null; echo $$?),0)
        USE_PARQUET := 1
    else
        USE_PARQUET := 0
    endif
endif

ifeq ($(USE_PARQUET),1)
    CXXFLAGS        += -DUSE_PARQUET $(shell pkg-config --cflags arrow parquet)
    PARQUET_LDLIBS  := $(shell pkg-config --libs arrow parquet)
else
    PARQUET_LDLIBS  :=
endif

BPF_CFLAGS = -O2 -g -target bpf -D__TARGET_ARCH_x86 -I/usr/include -I/usr/include/x86_64-linux-gnu -I.

LIBBPF_LD = -lbpf

all: lat-tracer ebpf-msg-flow

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

lat-tracer.bpf.o: lat-tracer.bpf.c vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

lat-tracer.skel.h: lat-tracer.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

lat-tracer: lat-tracer.cpp lat-tracer.skel.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBBPF_LD)

ebpf-msg-flow.bpf.o: ebpf-msg-flow.bpf.c vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

ebpf-msg-flow.skel.h: ebpf-msg-flow.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

ebpf-msg-flow: ebpf-msg-flow.cpp ebpf-msg-flow.skel.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBBPF_LD) $(PARQUET_LDLIBS)

clean:
	rm -f lat-tracer.bpf.o lat-tracer.skel.h lat-tracer vmlinux.h
	rm -f ebpf-msg-flow.bpf.o ebpf-msg-flow.skel.h ebpf-msg-flow

.PHONY: all clean
