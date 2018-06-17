PROG=		sigbak
SRCS=		backup.pb-c.c sbk.c sigbak.c
CLEANFILES=	backup.pb-c.c backup.pb-c.h
NOMAN=

COPTS+!=	pkg-config --cflags libcrypto libprotobuf-c sqlite3
LDADD+!=	pkg-config --libs libcrypto libprotobuf-c sqlite3

backup.pb-c.c backup.pb-c.h: backup.proto
	protoc-c --c_out=. backup.proto

sbk.o: backup.pb-c.h

.include <bsd.prog.mk>
