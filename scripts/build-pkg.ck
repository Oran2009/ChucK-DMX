@import "Chumpinate"

// instantiate a Chumpinate package
Package pkg("ChuGUI");

// add our metadata here
["Ben Hoang"] => pkg.authors;

"https://ccrma.stanford.edu/~hoangben/ChucK-DMX/" => pkg.homepage;
"https://github.com/Oran2009/ChucK-DMX/" => pkg.repository;

"ChucK-DMX: A plugin for ChucK that enables the sending of DMX over serial or over ethernet via the ArtNet and sACN network protocols." => pkg.description;
"MIT" => pkg.license;

["DMX", "lighting", "light bulbs", "ArtNet", "sACN", "Serial"] => pkg.keywords;

"./" => pkg.generatePackageDefinition;

PackageVersion ver("DMX", "0.1.0");

"1.5.5.0" => ver.languageVersionMin;

"any" => ver.os;
"all" => ver.arch;

ver.addFile("../build/Release/DMX.chug");

// wrap up all our files into a zip file, and tell Chumpinate what URL
// this zip file will be located at.
ver.generateVersion("../releases/" + ver.version(), "DMX", "https://ccrma.stanford.edu/~hoangben/ChucK-DMX/releases/" + ver.version() + "/DMX.zip");

ver.generateVersionDefinition("DMX", "./");