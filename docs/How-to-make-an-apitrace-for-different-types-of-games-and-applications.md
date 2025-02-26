# When apitrace can be useful:
1. Application has graphical defects/artifacts
2. Application freezes/causes gpu hangs (recoverable)
3. Application hangs your PC completely
4. Application crashes

# How to make an apitrace for **Windows** version of the game:

* Check whether the game is 64-bit or 32-bit.
To determine that on Linux, use `file` command in the terminal:
> file /path/to/executable/program.exe

where `/path/to/executable/` is an actual location of the game's EXE file and `program.exe` is its actual name.
If the game is **32-bit**, you will see this in the output
> PE32 executable (GUI) Intel 80386, for MS Windows

And if it's **64-bit**, you will get this:
> PE32+ executable (GUI) x86-64, for MS Windows

* **Determine api used by game:** **!!!!!!!!!!SEARCHING FOR A SOLUTION!!!!!!!!!!**


* Download prebuilt apitrace files: https://people.freedesktop.org/~jrfonseca/apitrace/apitrace-msvc-latest.7z
* Extract the archive.

If the game is **32-bit**, choose the file(s) located in 
>apitrace-msvc/**x86**/lib/wrappers

And if it's **64-bit**, choose the file(s) located in
>apitrace-msvc/**x64**/lib/wrappers
* Now copy that DLL file(s) into **the same folder as the game's .EXE**.
* **For DX11 game** Copy the appropriate `d3d11.dll`,`dxgi.dll`,`dxgitrace.dll` files

* **For DX9 game** Copy the appropriate `d3d9.dll` file
    <details><summary>SHOW ADDITIONAL STEPS</summary>

   1. For non-steam games open winecfg and add a native,builtin override for d3d9. Here's a GIF that showcases the process:
    
   ![exclude_d9vk](uploads/7673bd19d9249da50a02b1912e9cb5af/exclude_d9vk.gif)
   
   2. For Steam games
         * Open game preferences
         * Open "Set launch options"
         * Enter `WINEDLLOVERRIDES="d3d9=n,b" %command%`
         * Save

</details>

* Launch the game and try to reproduce your issue as fast as possible, then exit the game after that (using ALT+F4 is fine). **(note that game might be very slow, that's fine)**
* Locate the .trace file. In wine, you can usually find it in `/path/to/prefix/drive_c/users/alex/Desktop`, where `/path/to/prefix` is the location of your Wine prefix. For Steam - Wine prefix is located in `~/.steam/steam/steamapps/compatdata/'ID'/pfx` .

If you've done everything correctly, the file should have the size of at least a few hundred megabytes (or at least one hundred if the game is very simple), quite often it's over 1 GB. 
If the size is way less than that (like a few kilobytes), then it means the game is not really traceable, at least on Linux. Make sure you mention this in the issue.
* Compress the .trace file (preferably in something like `.tar.xz`) and share it using a *common* file sharing service like Google Drive or Mega.

# How to check created apitrace

To be sure that trace also can reproduce the problem, try to replay it locally:

>WINEPREFIX=<path_to_prefix> wine apitrace-msvc/**x64**/bin/apitrace.exe replay <path_to_trace.trace>

or

>WINEPREFIX=<path_to_prefix> wine apitrace-msvc/**x32**/bin/apitrace.exe replay <path_to_trace.trace>

**Note** for steam apps you should use same WINEPREFIX as was used by steam.



As expected result, you should also see your issue.


# How to make an apitrace for Linux version of the game (for **OpenGL** only):
1. Determine version of the game (x32 or x64)
2. Determine api used by game:
   * Launch the game
   * In cmd type 
       * `lsof -p <GAME_PID> | grep gl` for OpenGL api (if anything was found, game is using OpenGL)
       * `lsof -p <GAME_PID> | grep vulkan` for ANV api (if anything was found, game is using Vulkan)
3. Install "apitrace" package from your repo (or build it manually - https://github.com/apitrace/apitrace/blob/master/docs/INSTALL.markdown)

* For non-steam game:
   * Run the game from cmd, using the command:
>apitrace trace <game_binary>

* For steam game:
   * Open game preferences
   * Open "Set launch options"
   * Enter `apitrace trace %command%`
   * Save
   * Launch the game as usual for steam

4. Created apitrace will be placed into the directory with your executable


# In case if game is crashing/hanging and that's blocking you from making apitrace


**1.** The best solution for all games/applications to make apitrace, using [Windows OS](https://gitlab.freedesktop.org/GL/mesa/wikis/How-to-make-an-apitrace-using-Windows-OS)

General idea for next steps - for DX11 games you have try to make apitrace with/without DXVK. For DX9 games - to check with/without D9VK. 

More information about variables and possibilities can be found here:
DXVK https://github.com/doitsujin/dxvk
D9VK https://github.com/Joshua-Ashton/d9vk

**2.** DX11 games:
  * Steam version (disabling DXVK)
     * Open game preferences
     * Open "Set launch options"
     * Enter `PROTON_USE_WINED3D=1 %command%` (this will disable dxvk for the game and possibly, will allow to write apitrace without corruptions)

  * Non-steam version
     * Disable DXVK (as example, simply remove DXVK from your wineprefix (check the dxvk guides)

**3.** DX9 games:
  * Steam version (enabling D9VK)
     * Open game preferences
     * Open "Set launch options"
     * Enter `PROTON_USE_D9VK=1 %command%` (this will enable d9vk for the game and possibly, will allow to write apitrace without corruptions)

  * Non-steam version
     * Disable D9VK (as example, simply remove D9VK from your wineprefix (check the dxvk guides)


# I did everything correctly but *.trace file didn't appear

Mostly this case was found for steam titles, but who knows... Below you will find one more, "direct" way to make an apitrace
### For steam games
1. Open folder with Proton `<path/to/steam>/.steam/steam/steamapps/common/Proton 4.11`

**Note** - these steps are applicable for "Proton 4.11" (and higher, I think). In lower versions string for modification looks different (but quite similar).
2. Open file "proton" with text editor
3. Find line with
> self.run_proc([g_proton.wine_bin, "steam"] + sys.argv[2:])

4. Modify this line to:
* for **DX11** game
>self.run_proc([g_proton.wine_bin, "steam"] + ["/<some/path>/apitrace-msvc/x64/bin/apitrace.exe", "trace", "-a", "dxgi"] + sys.argv[2:])

* for **for DX9** game
>self.run_proc([g_proton.wine_bin, "steam"] + ["/<some/path>/apitrace-msvc/x64/bin/apitrace.exe", "trace", "-a", "dxd9"] + sys.argv[2:])

**Note** - `/apitrace-msvc/x64/bin/apitrace.exe` or `/apitrace-msvc/x86/bin/apitrace.exe` path should be taken, according to the version of the game for tracing.

5. Save the file
6. Launch the game as usual (from steam)

Trace will be created in your wine (game prefix), example `~/.local/share/Steam/steamapps/compatdata/354160/pfx/drive_c/users/steamuser/Desktop`, where `354160` is your gameID.

GameID can be found here https://steamdb.info/ (simply provide a game name and use search)

### For Non-steam games
1. Launch the game in cmd with the options:
* for **DX11** game
>WINEPREFIX=<path_to_prefix_with_DXVK> wine apitrace trace -a dxgi <game_binary>

* for **DX9** game
>WINEPREFIX=<path_to_prefix_with_D9VK> wine apitrace trace -a dxd9 <game_binary>

**Don't forget to revert all changes I did after taking apitrace**

  
  
  
  
Credits to https://github.com/Joshua-Ashton/d9vk/wiki/Making-a-Trace as a basis for the tutorial
