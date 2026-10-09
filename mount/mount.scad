// mount.scad - mount hardware for LoRa gate sensor
// Andrew Ho <andrew@zeuscat.com>

// Slop
s = 0.3;

// Protoboard (pb prefix)
pb_length        = 50.8+s;  // Measured long edge (along horizontal rows)
pb_width         = 43.2+s;  // Measured short edge (along vertical power rails)
pb_radius        =  2;      // Measured approximate edge radius
pb_thickness     =  1.5;    // Thickness of just the PCB itself
pb_gutter_width  =  1.8;    // Safe margin to avoid solder joints
pb_gutter_height =  2;      // Safe margin to avoid solder joints

// Battery holder (bh prefix)
bh_length        = 48.2+s;  // Short horizontal edge (along battery terminals)
bh_width         = 58.5+s;  // Long vertical edge (parallel to cells)
bh_height        = 16.4;    // Total depth from case bottom to top of cells
bh_side_height   =  7.0;    // Height from case bottom to finger gaps
bh_wire_offset   = 12.5+s;  // Distance from right edge that wires come out

// Our parameters
shell_thickness  =  1.5;    // Thickness of horizontal base plate
shell_radius     =  1;      // Horizontal radius like for finger gap
edge_thickness   =  2.5;    // Thickness around edge
edge_radius      =  2.5;    // Radius around edge
pb_center_gap    = 30;      // Leave this much room for finger prying

// Various derived parameters
edge_thickness2  = edge_thickness * 2;
pb_ext_length = pb_length + edge_thickness2;
pb_ext_width = pb_width + edge_thickness2;
pb_ext_height = shell_thickness + pb_gutter_height + pb_thickness;
bh_ext_length = bh_length + edge_thickness2;
bh_ext_width = bh_width + edge_thickness2;
bh_ext_height = shell_thickness + bh_side_height;

// Rendering parameters
e = 0.1;
e2 = e * 2;
$fn = 90;

// Rounded cube (corners rounded from x-y plane)
module rounded_cube_xy(length, width, height, radius) {
  module corner() {
    cylinder(r = radius, h = height);
  }
  hull() {
    translate([radius, radius]) corner();
    translate([radius, width - radius]) corner();
    translate([length - radius, width - radius]) corner();
    translate([length - radius, radius]) corner();
  }
}

// Rounded cube (corners rounded from x-z plane)
module rounded_cube_xz(length, width, height, radius) {
  translate([0, 0, height]) {
    rotate(-90, [1, 0, 0]) {
      rounded_cube_xy(length, height, width, radius);
    }
  }
}

// Cutout to create a gap with rounded edges
module gap_cutout(length, width, height, radius) {
  rounded_cube_xz(length, width, height, radius);
  difference() {
    translate([-radius, 0, height - radius]) {
      cube([length + (radius * 2), width, radius + e]);
    }
    translate([-(radius * 2), -e]) {
      rounded_cube_xz(radius * 2, width + e2, height, radius);
    }
    translate([length, -e]) {
      rounded_cube_xz(radius * 2, width + e2, height, radius);
    }
  }
}

// Sled that holds the protoboard
module pb_sled() {
  difference() {
    union() {
      difference() {
        rounded_cube_xy(pb_ext_length, pb_ext_width, pb_ext_height, edge_radius);
        translate([edge_thickness, edge_thickness, shell_thickness]) {
          rounded_cube_xy(pb_length, pb_width, pb_gutter_height + pb_thickness + e,
                          pb_radius);
        }
      }
      // Gutter that holds up the board, giving room for protruding solder joints
      translate([edge_thickness, edge_thickness, shell_thickness]) {
        pb_cavity_length = pb_length - (pb_gutter_width * 2);
        pb_cavity_width = pb_width - (pb_gutter_width * 2);
        difference() {
          rounded_cube_xy(pb_length, pb_width, pb_gutter_height, pb_radius);
          // Cut out inner cavity that directly holds the battery holder
          translate([pb_gutter_width, pb_gutter_width, -e]) {
            rounded_cube_xy(pb_cavity_length, pb_cavity_width,
                            pb_gutter_height + e2, pb_radius);
          }
        }
      }
    }
    // Cut out center gap to provide finger hold to pry out protoboard
    translate([(pb_ext_length - pb_center_gap) / 2, -e, shell_thickness]) {
      gap_cutout(pb_center_gap, pb_ext_width + e2,
                 pb_ext_height - shell_thickness, shell_radius);
    }
    // Cut out bottom void to save material and help poke out protoboard
    pb_void_length = pb_ext_length / 2;
    pb_void_width = pb_ext_width / 2;
    translate([(pb_ext_length - pb_void_length) / 2,
               (pb_ext_width - pb_void_width) / 2, -e]) {
      rounded_cube_xy(pb_void_length, pb_void_width, shell_thickness + e2,
                      edge_radius);
    }
  }
}

// Sled that holds the battery holder
module bh_sled() {
  difference() {
    rounded_cube_xy(bh_ext_length, bh_ext_width, bh_ext_height, edge_radius);
    translate([edge_thickness, edge_thickness, shell_thickness]) {
      cube([bh_length, bh_width, bh_side_height + e]);
    }
    // Cut out center gap to give wire space to come out the top
    bh_center_gap = bh_length - (2 * bh_wire_offset);
    translate([(bh_ext_length - bh_center_gap) / 2, -e, shell_thickness]) {
      gap_cutout(bh_center_gap, bh_ext_width + e2,
                 bh_ext_height - shell_thickness, shell_radius);
    }
    // Cut out bottom void to save material and print time
    bh_void_length = bh_ext_length / 2;
    bh_void_width = bh_ext_width / 2;
    translate([(bh_ext_length - bh_void_length) / 2,
               (bh_ext_width - bh_void_width) / 2, -e]) {
      rounded_cube_xy(bh_void_length, bh_void_width, shell_thickness + e2,
                      edge_radius);
    }
  }
}

// TODO: add some basic wire routing affordances

// The completed assembly that holds all components
module mount() {
  pb_sled();
  translate([pb_ext_length, 0]) bh_sled();
  // Undo the edge curves where the pieces meet
  difference() {
    translate([pb_ext_length - edge_thickness, 0]) {
      cube([edge_thickness2, pb_ext_width + edge_radius, pb_ext_height]);
    }
    // And cut out a nice curved join, just for aesthetics
    translate([pb_ext_length - edge_radius, pb_ext_width + edge_radius, -e]) {
      cylinder(r = edge_radius, h = pb_ext_height + e2);
    }
  }
}

mount();
