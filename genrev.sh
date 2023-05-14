#!/bin/bash
echo "#define KRONOS_HG_REVISION \"$(hg id -n)\""
echo "#define KRONOS_HG_BRANCH \"$(hg id -b)\""
echo "#define KRONOS_HG_TAGS \"$(hg parent -T {latesttag}~{latesttagdistance-1})\""