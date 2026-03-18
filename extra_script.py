Import("env")
import os

SDK_DRIVER = os.path.join(
    env.subst("$PROJECT_DIR"),
    "SDK", "DW3_QM33_SDK_1.1.1", "Drivers", "API", "Shared", "dwt_uwb_driver"
)

# Add include paths
env.Append(CPPPATH=[
    SDK_DRIVER,
    os.path.join(SDK_DRIVER, "dw3000"),
    os.path.join(SDK_DRIVER, "lib", "qmath", "include"),
])

# Build driver sources into a separate node so they link with the app
env.BuildSources(
    os.path.join("$BUILD_DIR", "dwt_uwb_driver"),
    SDK_DRIVER,
    src_filter=[
        "-<*>",
        "+<deca_interface.c>",
        "+<deca_compat.c>",
        "+<deca_rsl.c>",
        "+<dw3000/dw3000_device.c>",
        "+<lib/qmath/src/qmath.c>",
    ],
)
