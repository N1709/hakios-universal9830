/*
 * Registration host for the user's LocalStorageFileProvider extension.
 * Hidden from SpringBoard; no app UI or background process is needed.
 * Keeping the extension in an app bundle lets LaunchServices register its
 * existing data/app-group containers, which registerPlugin: alone omits.
 */
int main(void)
{
    return 0;
}
