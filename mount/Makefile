# Makefile - create STL files from OpenSCAD source
# Andrew Ho (andrew@zeuscat.com)

ifeq ($(shell uname), Darwin)
  OPENSCAD = /Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD
else
  OPENSCAD = openscad
endif

TARGETS = mount.stl

all: $(TARGETS)

mount.stl: mount.scad
	$(OPENSCAD) -o mount.stl mount.scad

clean:
	@rm -f $(TARGETS)
