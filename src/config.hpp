#pragma once

// Persistent user settings (settings.ini next to the exe) and the example
// routes.json bootstrap.

#include <string>

struct Settings {
    int port = 5555;
    std::string address = "0.0.0.0";  // bind address: 0.0.0.0 = all interfaces
    std::string root;                 // static file root; empty -> exe_dir\www
};

// Directory of the running executable, without trailing slash.
std::string ExeDir();

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Path of the routes file the tool reads (exe_dir\routes.json).
std::string RoutesFilePath();

// Writes a small example routes.json next to the exe when it does not exist
// yet, so first-time users see a working example immediately.
void EnsureExampleRoutesFile();
