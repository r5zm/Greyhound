#pragma once

#include <string>

// We need to include external libraries for models
#include "WraithModel.h"
#include "WraithAnim.h"

// A class that handles writing SMD model files
class ValveSMD
{
public:
    // Export a WraithModel to a Valve SMD file
    static void ExportSMD(const WraithModel& Model, const std::string& FileName);

    // Legacy standalone animation export
    static void ExportSMD(const WraithAnim& Animation, const std::string& FileName);

    // Correct animation export using a reference skeleton/model
    static void ExportSMD(const WraithAnim& Animation, const WraithModel& ReferenceModel, const std::string& FileName);
};