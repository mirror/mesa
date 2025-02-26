If you have access to Windows, it's best that you perform the tracing on it rather than on Linux.
* Check whether the game is 64-bit or 32-bit. 
To do that on Windows, right-click on the executable file you want to check, select “Properties”, then click the tab “Compatibility”.

If the first Windows version in the list is "Windows Vista", then it means the game is **64-bit**.
 
If you see older Windows versions in that menu, like "Windows 98", then the game is **32-bit**.
![](https://cdn.discordapp.com/attachments/545938151739228191/604369144892358667/compatibility-check.png)
* Download prebuilt apitrace files: https://people.freedesktop.org/~jrfonseca/apitrace/apitrace-msvc-latest.7z
* Extract the archive. 
* **For DirectX11 games** you have to copy the appropriate `d3d11.dll`,`dxgi.dll`,`dxgitrace.dll` files 
* **For DirectX9 games** you have to copy the appropriate `d3d9.dll` file.

If the game is **32-bit**, choose the file(s) located in 
>apitrace-msvc/**x86**/lib/wrappers

And if it's **64-bit**, choose the file(s) located in
>apitrace-msvc/**x64**/lib/wrappers
* Now copy that DLL file(s) into **the same folder as the game's .EXE**.
* Launch the game and try to reproduce your issue as fast as possible, then exit the game after that (using ALT+F4 is fine) **(note that game might be very slow, that's fine)**
* The .trace file should be located on your Desktop.
If you've done everything correctly, the file should have the size of at least a few hundred megabytes (or at least one hundred if the game is very simple), quite often it's over 1 GB. 
If the size is way less than that (like a few kilobytes), then it means the game is not really traceable, at least on Linux. Make sure you mention this in the issue.
* Compress the .trace file (preferably in something like `.7z`) and share it using a *common* file sharing service like Google Drive or Mega.


Copyrights,
https://github.com/Joshua-Ashton/d9vk/wiki/Making-a-Trace