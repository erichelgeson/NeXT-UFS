CC	?= cc
CFLAGS	?= -O2 -g
CFLAGS	+= -std=c99 -Wall -Wextra -Werror -Iinclude -D_GNU_SOURCE
OBJDIR	= build
LIBSRCS	= src/util.c src/label.c src/apm.c src/fs.c src/alloc.c src/write.c \
	  src/inode.c src/dir.c src/dirops.c src/mkfs.c src/fsck.c src/path.c
LIBOBJS	= $(LIBSRCS:src/%.c=$(OBJDIR)/%.o)
BIN	= $(OBJDIR)/nextufs

# The FUSE driver is a second binary and the only thing that needs a library
# off this machine, so plain `make` never looks for one. `make fuse` does.
FUSE_BIN    = $(OBJDIR)/nextufs-fuse
FUSE_CFLAGS = $(subst -I,-isystem,$(shell pkg-config --cflags fuse3 2>/dev/null))
FUSE_LIBS   = $(shell pkg-config --libs fuse3 2>/dev/null)

all: $(BIN)

$(BIN): $(LIBOBJS) $(OBJDIR)/main.o
	$(CC) $(CFLAGS) -o $@ $^

fuse: $(FUSE_BIN)

$(FUSE_BIN): $(LIBOBJS) $(OBJDIR)/fuse.o
	$(CC) $(CFLAGS) -o $@ $^ $(FUSE_LIBS)

$(OBJDIR)/fuse.o: src/fuse.c include/nextufs.h | $(OBJDIR)
	@pkg-config --exists fuse3 || \
	    { echo "fuse3 not found; try: nix-shell -p pkg-config fuse3 --run 'make fuse'"; exit 1; }
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -DFUSE_USE_VERSION=31 -c -o $@ $<

$(OBJDIR)/%.o: src/%.c include/nextufs.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR)

.PHONY: all fuse clean
