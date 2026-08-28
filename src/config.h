#pragma once
// Persisted settings, stored as key=value text in
// %APPDATA%\openmix\config.ini.
//
// Plain text on purpose: it is greppable, diffable, hand-editable when
// something goes wrong, and needs no dependency.

#include <map>
#include <string>

class Config {
public:
    // Loads the file if present. Missing file is not an error -- defaults win.
    void load();
    // Whether a settings file was found. False means this is a first run.
    bool existed() const { return existed_; }
    bool save() const;

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    float getFloat(const std::string& key, float fallback) const;
    bool getBool(const std::string& key, bool fallback) const;

    void set(const std::string& key, const std::string& value);
    void setFloat(const std::string& key, float value);
    void setBool(const std::string& key, bool value);

    static std::string path();

private:
    std::map<std::string, std::string> values_;
    bool existed_ = false;
};

// Start openmix when the user signs in, via the HKCU Run key. Registering
// there rather than a scheduled task keeps it per-user and needs no admin.
bool autostartEnabled();
bool setAutostart(bool enable);
