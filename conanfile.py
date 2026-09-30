from conan import ConanFile


class DesktopCompanionConan(ConanFile):
    name = "desktop-companion"
    version = "0.1.0"
    settings = "os", "arch", "compiler", "build_type"
    requires = "glew/2.2.0"
    generators = "CMakeDeps"

    # Qt and the proprietary Cubism SDK are supplied locally.
