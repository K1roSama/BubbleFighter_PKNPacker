"""Generate native VS projects and matching filters from the project source tree."""
from pathlib import Path
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = "http://schemas.microsoft.com/developer/msbuild/2003"
ET.register_namespace("", NS)


def node(parent, tag, value=None, **attributes):
    result = ET.SubElement(parent, "{" + NS + "}" + tag, attributes)
    if value is not None:
        result.text = str(value)
    return result


def guid(name):
    return "{" + str(uuid.uuid5(uuid.NAMESPACE_URL, "PKNPacker/" + name)).upper() + "}"


def save(tree, path):
    # Python 3.8 has no ET.indent.
    from xml.dom import minidom
    output = minidom.parseString(ET.tostring(tree, encoding="utf-8")).toprettyxml(indent="  ", encoding="utf-8")
    path.write_bytes(output.replace(b"\r\n", b"\n"))


def project(name, folder, kind, qt=False):
    p = ET.Element("{" + NS + "}Project", {"DefaultTargets": "Build", "ToolsVersion": "Current"})
    configs = node(p, "ItemGroup", Label="ProjectConfigurations")
    for configuration in ["Debug", "Release"]:
        cfg = node(configs, "ProjectConfiguration", Include=configuration + "|x64")
        node(cfg, "Configuration", configuration); node(cfg, "Platform", "x64")
    globals_ = node(p, "PropertyGroup", Label="Globals")
    node(globals_, "ProjectGuid", guid(name))
    node(globals_, "Keyword", "QtVS_v304" if qt else "Win32Proj")
    node(globals_, "RootNamespace", name)
    node(globals_, "WindowsTargetPlatformVersion", "10.0.26100.0")
    node(p, "Import", Project="..\\..\\Build\\Dependencies.props")
    node(p, "Import", Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props")
    for configuration in ["Debug", "Release"]:
        cfg = node(p, "PropertyGroup", Label="Configuration", Condition="'$(Configuration)|$(Platform)'=='" + configuration + "|x64'")
        node(cfg, "ConfigurationType", kind)
        node(cfg, "UseDebugLibraries", "true" if configuration == "Debug" else "false")
        node(cfg, "PlatformToolset", "v143")
        node(cfg, "CharacterSet", "Unicode")
    if qt:
        node(p, "Import", Project="$(QtMsBuild)\\qt_defaults.props", Condition="Exists('$(QtMsBuild)\\qt_defaults.props')")
    node(p, "Import", Project="$(VCTargetsPath)\\Microsoft.Cpp.props")
    node(p, "Import", Project="..\\..\\Build\\Common.props")
    if qt:
        settings = node(p, "PropertyGroup", Label="QtSettings")
        node(settings, "QtInstall", "$(QtRoot)")
        node(settings, "QtModules", "core;gui;widgets;testlib")
        node(p, "Import", Project="$(QtMsBuild)\\qt.props", Condition="Exists('$(QtMsBuild)\\qt.props')")
        defs = node(p, "ItemDefinitionGroup")
        link = node(defs, "Link"); node(link, "SubSystem", "Windows"); node(link, "EntryPointSymbol", "mainCRTStartup")
    elif kind != "StaticLibrary":
        defs = node(p, "ItemDefinitionGroup")
        link = node(defs, "Link"); node(link, "SubSystem", "Console")
    items = node(p, "ItemGroup")
    mapping = []
    for file in sorted(folder.rglob("*")):
        if not file.is_file():
            continue
        extension = file.suffix.lower()
        itemtype = {".cpp": "ClCompile", ".h": "ClInclude", ".ui": "QtUic", ".qrc": "QtRcc"}.get(extension)
        if not itemtype:
            continue
        if qt and file.name == "MainWindow.h":
            itemtype = "QtMoc"
        relative = str(file.relative_to(folder)).replace("/", "\\")
        node(items, itemtype, Include=relative)
        mapping.append((itemtype, relative, str(file.parent.relative_to(folder)).replace("/", "\\")))
    if kind == "StaticLibrary":
        for file in sorted((ROOT / "ThirdParty/Zlib").glob("*.c")):
            item = node(items, "ClCompile", Include="..\\..\\ThirdParty\\Zlib\\" + file.name)
            node(item, "WarningLevel", "Level1")
            node(item, "CompileAs", "CompileAsC")
            mapping.append(("ClCompile", "..\\..\\ThirdParty\\Zlib\\" + file.name, "ThirdParty\\Zlib"))
    else:
        refs = node(p, "ItemGroup")
        reference = node(refs, "ProjectReference", Include="..\\..\\Source\\PKNPacker.Core\\PKNPacker.Core.vcxproj")
        node(reference, "Project", guid("PKNPacker.Core"))
    node(p, "Import", Project="$(VCTargetsPath)\\Microsoft.Cpp.targets")
    if qt:
        node(p, "Import", Project="$(QtMsBuild)\\qt.targets", Condition="Exists('$(QtMsBuild)\\qt.targets')")
        error = node(p, "Target", Name="RequireQt", BeforeTargets="PrepareForBuild")
        node(error, "Error", Condition="!Exists('$(QtMsBuild)\\qt.targets') or !Exists('$(QtRoot)\\bin\\qmake.exe')",
             Text="Qt dependencies missing. Run Tools/SetupDependencies.py or configure Build/Local.props.")
    save(p, folder / (name + ".vcxproj"))
    f = ET.Element("{" + NS + "}Project", {"ToolsVersion": "4.0"})
    filters = sorted(set(category for _, _, category in mapping if category))
    group = node(f, "ItemGroup")
    for category in filters:
        item = node(group, "Filter", Include=category); node(item, "UniqueIdentifier", guid(name + "/" + category))
    group = node(f, "ItemGroup")
    for itemtype, relative, category in mapping:
        item = node(group, itemtype, Include=relative)
        if category:
            node(item, "Filter", category)
    save(f, folder / (name + ".vcxproj.filters"))
    return folder.relative_to(ROOT) / (name + ".vcxproj")


def main():
    entries = []
    for name, relative, kind, qt in [
        ("PKNPacker.Core", "Source/PKNPacker.Core", "StaticLibrary", False),
        ("PKNPacker", "Source/PKNPacker", "Application", True),
        ("PKNPacker.Cli", "Source/PKNPacker.Cli", "Application", False),
        ("PKNPacker.Tests", "Tests/PKNPacker.Tests", "Application", False)]:
        folder = ROOT / relative
        folder.mkdir(parents=True, exist_ok=True)
        entries.append((name, project(name, folder, kind, qt)))
    lines = ["Microsoft Visual Studio Solution File, Format Version 12.00", "# Visual Studio Version 17",
             "VisualStudioVersion = 17.14.36216.28", "MinimumVisualStudioVersion = 10.0.40219.1"]
    for name, path in entries:
        lines += ['Project("{BC8A1FFA-BEE3-4634-8014-F334798102B3}") = "%s", "%s", "%s"' % (name, str(path).replace("/", "\\"), guid(name)), "EndProject"]
    lines += ["Global", "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution", "\t\tDebug|x64 = Debug|x64", "\t\tRelease|x64 = Release|x64", "\tEndGlobalSection", "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution"]
    for name, _ in entries:
        for configuration in ["Debug", "Release"]:
            for suffix in ["ActiveCfg", "Build.0"]:
                lines.append("\t\t%s.%s|x64.%s = %s|x64" % (guid(name), configuration, suffix, configuration))
    lines += ["\tEndGlobalSection", "EndGlobal", ""]
    (ROOT / "PKNPacker.sln").write_text("\n".join(lines), encoding="utf-8")
    print("Four native VS projects and filters generated")


if __name__ == "__main__":
    main()
