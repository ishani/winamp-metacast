
-- ------------------------------------------------------------------------------
workspace ("gen_metacast_" .. _ACTION)

    configurations  { "Debug", "Release" }
    platforms       { "x86" }

    filter "platforms:x86"
        architecture    "x86"
        system          "windows"
        defines {
           "WIN32",
           "_WINDOWS",
        }
        vectorextensions "SSE2"

    filter {}

    floatingpoint         "Fast"

    filter "configurations:Debug"
        defines   { "DEBUG" }
        symbols   "On"

    filter "configurations:Release"
        defines   { "NDEBUG" }
        flags     { "LinkTimeOptimization" }
        optimize  "Full"

    filter {}


-- ------------------------------------------------------------------------------
project "gen_metacast"
    kind "SharedLib"
    language "C++"

    objdir      ( "$(SolutionDir)_obj/%{cfg.shortname}/$(ProjectName)/" )
    debugdir    ( "$(OutDir)" )
    targetdir   ( "$(SolutionDir)_builds/$(Configuration)/%{cfg.platform}" )

    links
    {
        "shlwapi",
        "version.lib",
        "setupapi.lib",
    }

    includedirs { "sdk" }
    files {
        "sdk/**.h",
        "*.cpp",
        "*.h",
        "*.inl",
    }
