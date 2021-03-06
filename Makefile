CC = g++

#EXTRA_CFLAGS = `sdl-config --cflags`
#EXTRA_LIBS = `sdl-config --libs`

CC = gcc-4.5
CXX = g++-4.5
OPT = -O -DNDEBUG
#OPT = -g -Wall

D =
#D = d

OBJS = channel.o config.o configEvent.o error.o frameData.o initData.o main.o node.o eqEarth.o pipe.o view.o window.o renderer.o sceneView.o viewer.o controls.o earthManipulator.o
CFLAGS = -DEQ_IGNORE_GLEW ${OPT} -I/afs/cmf/project/dc/sys/boost/include -isystem /afs/cmf/project/dc/sys/include -I/afs/cmf/project/dc/sys/include -I/afs/cmf/project/gis/include ${EXTRA_CFLAGS} -I.
#CFLAGS = -DEQ_IGNORE_GLEW ${OPT} -I/var/tmp/dkleiner/dev/Buildyard/Build/install/include -isystem /afs/cmf/project/dc/sys/include -I/afs/cmf/project/dc/sys/include -I/afs/cmf/project/gis/include ${EXTRA_CFLAGS} -I.
LIBS = -Wl,-rpath -Wl,/afs/cmf/project/dc/sys/boost/lib -L/afs/cmf/project/dc/sys/boost/lib -lboost_serialization -lboost_system -lboost_date_time -L/afs/cmf/project/dc/sys/lib -losg${D} -losgViewer${D} -losgUtil${D} -lEqualizer -L/afs/cmf/project/gis/lib -losgEarth${D} -losgEarthUtil${D} ${EXTRA_LIBS}
#LIBS = -Wl,-rpath -Wl,/var/tmp/dkleiner/dev/Buildyard/Build/install/lib -L/var/tmp/dkleiner/dev/Buildyard/Build/install/lib -L/afs/cmf/project/dc/sys/lib -losg${D} -losgViewer${D} -losgUtil${D} -lEqualizer -L/afs/cmf/project/gis/lib -losgEarth${D} -losgEarthUtil${D} ${EXTRA_LIBS}

all: eqEarth

eqEarth: ${OBJS}
	${CC} ${CFLAGS} -o $@ ${OBJS} ${LIBS}

clean:
	/bin/rm -f *.o eqEarth

.SUFFIXES: 
.SUFFIXES: .o .cpp

.cpp.o:
	${CC} ${CFLAGS} -c $<
