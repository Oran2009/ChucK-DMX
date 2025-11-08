//---------------------------------------------------------------------
// name: gen-dmx.ck
// desc: generate HTML documentation for ChucK-DMX
//
// author: Ben Hoang (https://ccrma.stanford.edu/~hoangben/)
// date: Fall 2025
//---------------------------------------------------------------------

// instantiate a CKDoc object
CKDoc doc; // documentation orchestra
// set the examples root
"../examples/" => doc.examplesRoot;

// add group
doc.addGroup(
    // class names
    [
        "DMX"
    ],
    // group name
    "DMX",
    // file name
    "dmx",
    // group description
    "The DMX class provides control over DMX512 lighting data and protocol selection for ChucK."
);

// sort for now until order is preserved by CKDoc
doc.sort(true);

// generate
doc.outputToDir("../../../ccrma/home/Web/ChucK-DMX/api/", "ChucK-DMX API Reference (v0.1.0)");
