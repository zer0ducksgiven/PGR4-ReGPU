#pragma once
#include <filesystem>

// Starts the runtime services the title expects from the console: the
// timestamp bundle, the file system roots, the fake GPU and audio clocks.
void RuntimeInit(const std::filesystem::path& gameDir);

const std::filesystem::path& GameDir();
const std::filesystem::path& UserDir();
// The signed-in profile's saves (\Device\Content).
const std::filesystem::path& ContentDir();
const std::filesystem::path& ContentDir2(); // Player 2's saves (split screen)
// Downloadable content shared by every profile (\Device\Dlc; one folder per package).
const std::filesystem::path& DlcDir();
