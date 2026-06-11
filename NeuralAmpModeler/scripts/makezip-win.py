import zipfile, os, sys, shutil

scriptpath = os.path.dirname(os.path.realpath(__file__))
projectpath = os.path.abspath(os.path.join(scriptpath, os.pardir))

iplug2_scripts = os.path.abspath(os.path.join(projectpath, os.pardir, "iPlug2", "Scripts"))

sys.path.insert(0, iplug2_scripts)

from get_archive_name import get_archive_name
from parse_config import parse_config


def add_file(zf, path, arcname=None):
    if not os.path.isfile(path):
        raise FileNotFoundError(path)
    zf.write(path, arcname or os.path.basename(path), zipfile.ZIP_DEFLATED)


def add_dir(zf, path, arcroot=None):
    if not os.path.isdir(path):
        raise FileNotFoundError(path)
    arcroot = arcroot or os.path.basename(path)
    for root, _, filenames in os.walk(path):
        for filename in filenames:
            fullpath = os.path.join(root, filename)
            relpath = os.path.relpath(fullpath, path)
            zf.write(fullpath, os.path.join(arcroot, relpath), zipfile.ZIP_DEFLATED)


def main():
    if len(sys.argv) != 3:
        print("Usage: make_zip.py demo[0/1] zip[0/1]")
        sys.exit(1)
    else:
        demo = int(sys.argv[1])
        zip = int(sys.argv[2])

    config = parse_config(projectpath)
    bundle_name = config["BUNDLE_NAME"]
    project_name = os.path.basename(projectpath)

    dir = os.path.join(projectpath, "build-win", "out")

    if os.path.exists(dir):
        shutil.rmtree(dir)

    os.makedirs(dir)

    files = []

    if not zip:
        installer_name = bundle_name + " Installer.exe"
        installer = os.path.join("build-win", "installer", installer_name)

        if demo:
            installer = os.path.join("build-win", "installer", bundle_name + " Demo Installer.exe")

        manual = os.path.join(projectpath, "manual", bundle_name + " manual.pdf")
        if not os.path.isfile(manual):
            manual = os.path.join(projectpath, "manual", project_name + " manual.pdf")

        files = [
            os.path.join(projectpath, installer),
            os.path.join(projectpath, "installer", "changelog.txt"),
            os.path.join(projectpath, "installer", "known-issues.txt"),
            manual,
        ]
    else:
        files = [
            os.path.join(projectpath, "build-win", bundle_name + ".vst3"),
            os.path.join(projectpath, "build-win", bundle_name + "_x64.exe"),
        ]

    zipname = get_archive_name(projectpath, "win", "demo" if demo == 1 else "full")

    zf = zipfile.ZipFile(
        projectpath + "\\build-win\\out\\" + zipname + ".zip", mode="w"
    )

    for f in files:
        print("adding " + f)
        if os.path.isdir(f):
            add_dir(zf, f)
        else:
            add_file(zf, f)

    zf.close()
    print("wrote " + zipname)

    zf = zipfile.ZipFile(
        projectpath + "\\build-win\\out\\" + zipname + "-pdbs.zip", mode="w"
    )

    pdb_dir = os.path.join(projectpath, "build-win", "pdbs")
    files = []
    if os.path.isdir(pdb_dir):
        files = [
            os.path.join(pdb_dir, filename)
            for filename in os.listdir(pdb_dir)
            if filename.lower().endswith(".pdb")
        ]

    if not files:
        print("No PDB files found in " + pdb_dir)

    for f in files:
        print("adding " + f)
        add_file(zf, f)

    zf.close()
    print("wrote " + zipname)


if __name__ == "__main__":
    main()
