from conan import ConanFile


class DesktopCompanionConan(ConanFile):
    name = "desktop-companion"
    version = "0.1.0"
    settings = "os", "arch", "compiler", "build_type"

    # Qt is supplied by the user's existing local installation.
    # Keep Conan available for third-party dependencies when they are added.
