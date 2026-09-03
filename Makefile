CC	?= cc
CFLAGS	?= -O2 -g
CFLAGS	+= -std=c99 -Wall -Wextra -Werror -Iinclude -D_GNU_SOURCE
OBJDIR	= build
SRCS	= src/util.c src/label.c src/fs.c src/alloc.c src/write.c src/inode.c src/dir.c src/dirops.c src/mkfs.c src/fsck.c \
	  src/path.c src/main.c
OBJS	= $(SRCS:src/%.c=$(OBJDIR)/%.o)
BIN	= $(OBJDIR)/nextufs

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(OBJDIR)/%.o: src/%.c include/nextufs.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR)

.PHONY: all clean
