# cmake/PatchJuceForMSVC.cmake
# ---------------------------------------------------------------------------
# juce_PushNotifications.h defines enum values whose names are Windows macros
# (none, small, large, min, max, normal, high from winnt.h / windef.h).
# This causes MSVC parse errors in the juce_gui_extra module.
# MorphSnap doesn't use PushNotifications, but the class is always parsed.
# This script renames the offending identifiers to safe alternatives.
# Must be called AFTER FetchContent_MakeAvailable(juce).
# ---------------------------------------------------------------------------

if(WIN32 AND MSVC)
    set(_JUCE_PUSH_NOTIF_H
        "${juce_SOURCE_DIR}/modules/juce_gui_extra/misc/juce_PushNotifications.h"
    )
    set(_JUCE_PUSH_NOTIF_CPP_ANDROID
        "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/juce_PushNotifications_android.cpp"
    )

    if(EXISTS "${_JUCE_PUSH_NOTIF_H}")
        file(READ "${_JUCE_PUSH_NOTIF_H}" _CONTENT)

        # Only patch if not already patched (idempotent check)
        if(NOT _CONTENT MATCHES "noBadge")
            message(STATUS "MorphSnap: Patching juce_PushNotifications.h for MSVC Windows macro conflicts")

            # BadgeIconType enum: none -> noBadge, small -> smallBadge, large -> largeBadge
            string(REGEX REPLACE "(enum BadgeIconType[ \t\r\n]*\\{[ \t\r\n]*)none"
                                 "\\1noBadge" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum BadgeIconType[ \t\r\n]*\\{[ \t\r\n]*noBadge,[ \t\r\n]*)small"
                                 "\\1smallBadge" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum BadgeIconType[ \t\r\n]*\\{[ \t\r\n]*noBadge,[ \t\r\n]*smallBadge,[ \t\r\n]*)large"
                                 "\\1largeBadge" _CONTENT "${_CONTENT}")
            string(REPLACE "BadgeIconType badgeIconType = large" "BadgeIconType badgeIconType = largeBadge" _CONTENT "${_CONTENT}")

            # Channel::Importance enum: none/min/low/normal/high/max -> prefixed names
            string(REGEX REPLACE "(enum Importance[ \t\r\n]*\\{[ \t\r\n]*)none"
                                 "\\1importanceNone" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum Importance[ \t\r\n]*\\{[^}]*)([^a-zA-Z])min([^a-zA-Z])"
                                 "\\1\\2importanceMin\\3" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum Importance[ \t\r\n]*\\{[^}]*)([^a-zA-Z])low([^a-zA-Z])"
                                 "\\1\\2importanceLow\\3" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum Importance[ \t\r\n]*\\{[^}]*)([^a-zA-Z])normal([^a-zA-Z])"
                                 "\\1\\2importanceNormal\\3" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum Importance[ \t\r\n]*\\{[^}]*)([^a-zA-Z])high([^a-zA-Z])"
                                 "\\1\\2importanceHigh\\3" _CONTENT "${_CONTENT}")
            string(REGEX REPLACE "(enum Importance[ \t\r\n]*\\{[^}]*)([^a-zA-Z])max([^a-zA-Z])"
                                 "\\1\\2importanceMax\\3" _CONTENT "${_CONTENT}")
            string(REPLACE "Importance importance = normal" "Importance importance = importanceNormal" _CONTENT "${_CONTENT}")

            file(WRITE "${_JUCE_PUSH_NOTIF_H}" "${_CONTENT}")
            message(STATUS "MorphSnap: juce_PushNotifications.h patched successfully")
        else()
            message(STATUS "MorphSnap: juce_PushNotifications.h already patched, skipping")
        endif()
    endif()
endif()
