#pragma once
#include "model.h"
#include <iosfwd>
#include <string>

bool saveProjectFile(const Project& project, const std::string& path, std::string& error);
bool loadProjectFile(Project& project, const std::string& path, std::string& error);

// In-memory form of the same format, used for undo / redo snapshots.
std::string serializeProject(const Project& project);
bool deserializeProject(Project& project, const std::string& data, std::string& error);
