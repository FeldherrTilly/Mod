@echo off
cd /d "%~dp0"
SET PATH=C:\Strawberry\perl\bin;%PATH%;C:\GOG Games\Civilization IV Complete\Civ4Colonization\Mods\Compiler\Microsoft Visual C++ Toolkit 2003\bin
set TARGET=Release
set BL="BOOST_LIB_PATH=C:/GOG Games/Civilization IV Complete/Civ4Colonization/Mods/Compiler/lib/"
bin\jom source_list /NOLOGO %BL% && nmake precompile /NOLOGO %BL% && bin\jom build %BL%
