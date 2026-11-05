/* SPDX-License-Identifier: GPL-3.0-or-later
 * Link stub for the system's libSceCommonDialog, which the public SDK has none of: it only names the imports the title makes
 * (ps5/game/build.sh builds it and hands it to the link and to ps5-native-tool). Never packaged or run. */
int sceCommonDialogInitialize(void) { return -1; }
int sceCommonDialogIsUsed(void) { return 0; }
