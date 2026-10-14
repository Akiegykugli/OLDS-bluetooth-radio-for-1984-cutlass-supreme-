# PlatformIO extra script: define BUILD_EPOCH = UTC seconds at build time.
# main.cpp uses it as a fallback time if the DS3231 lost power or reads garbage.
# It is applied to the project sources only (projenv), so the big libraries
# (Bluetooth etc.) are not recompiled at every build.
Import("env")
import time

define = ("BUILD_EPOCH", "%dUL" % int(time.time()))

try:
    Import("projenv")
    projenv.Append(CPPDEFINES=[define])
except Exception:
    # fallback: apply to everything (still works, just rebuilds more)
    env.Append(CPPDEFINES=[define])