#include "hgrev.h"

#ifndef KRONOS_HG_REVISION
#error Missing revision!
#endif

const char *BuildBranch = KRONOS_HG_BRANCH;
const char *BuildTags = KRONOS_HG_TAGS;
const char *BuildRevision = KRONOS_HG_REVISION;

const char *BuildIdentifier =
    KRONOS_HG_BRANCH "(" KRONOS_HG_TAGS "):" KRONOS_HG_REVISION;
