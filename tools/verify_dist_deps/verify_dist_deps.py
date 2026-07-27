import argparse
from collections import defaultdict
import json
import logging
import os
import platform
import re
import sys
from zipfile import ZipFile

from third_party.python.packaging import metadata
from third_party.python.packaging.utils import canonicalize_name

logger = logging.getLogger(__name__)

python_version = platform.python_version()


def build_file_extension_re(exts: list[str]):
    return re.compile(r"\.(" + "|".join([re.escape(e) for e in exts]) + ")$")

def build_metadata_file_re(module_dir: str | None = None):
    # Only match .dist-info directories at the top level or immediately beneath module_dir.
    r = r"[^/]+\.dist-info/METADATA$"
    if module_dir:
        r = "^(" + re.escape(module_dir.rstrip("/")) + "/)?" + r
    return re.compile(r)

def find_dist_archives(path: str, ext_match: re.Pattern | None = None, exclude: list = []):
    def ignore(f):
        for exc in exclude:
            if os.path.samefile(f, exc):
                return True
        if ext_match and not ext_match.search(f):
            return True
        return False
        
    for d, _, files in os.walk(path):
        for f in files:
            p = os.path.join(d, f)
            if ignore(p):
                continue
            yield p

def parse_dist_metadata(s: str | bytes):
    raw, _ = metadata.parse_email(s)
    return metadata.Metadata.from_raw(raw, validate=False)

def get_dist_metadata(archive: str, module_dir: str | None = None):
    metadata_re = build_metadata_file_re(module_dir)
    with ZipFile(archive, "r") as z:
        for info in z.infolist():
            if info.is_dir() or not metadata_re.fullmatch(info.filename):
                continue
            logger.debug(f"{archive}: parsing distribution metadata file {info.filename}")
            meta = parse_dist_metadata(z.read(info))
            logger.debug(f"{archive} provides {meta.name} {meta.version}")
            yield meta

def verify_requirements(
    targets: list[metadata.Metadata],
    extras: dict[str, list[str]],
    deps: dict[str, dict[str, str | metadata.Metadata]],
):
    unmet = []
    unused = set([dist["file"] for dep in deps.values() for dist in dep])

    def _verify_requirements(target: metadata.Metadata, extra: str, dist_stack: list[str]):
        dist_stack = dist_stack + [target]

        # Ensure Requires-Python field is satisifed, if there is one.
        if target.requires_python and python_version not in target.requires_python:
            unmet.append((dist_stack, target.requires_python, "Python version not satisfied"))
            return
        # Ensure all Requires-Dist fields are satisfied, if there are any.
        if target.requires_dist:
            for req in target.requires_dist:
                # If this requirement has an environment marker that evaluates to false, the
                # requirement need not be satisfied, so don't continue.
                if req.marker and not req.marker.evaluate({"extra": extra}):
                    continue
                req_name = str(canonicalize_name(req.name))
                if req_name not in deps:
                    unmet.append((dist_stack, req, "no distribution with this name"))
                    continue
                # Verify that at least one of the distributions in deps with this name satisfies the
                # given version constraint.
                try:
                    chosen = next(filter(lambda d: d["metadata"].version in req.specifier, deps[req_name]))
                except StopIteration:
                    unmet.append((dist_stack, req, "version specifier not satisfied"))
                    continue
                # Recursively verify the requirements of the chosen distribution, taking into
                # consideration any extras specified in the parent requirement.
                if req.extras:
                    for e in req.extras:
                        _verify_requirements(chosen["metadata"], e, dist_stack)
                else:
                    _verify_requirements(chosen["metadata"], "", dist_stack)
                try:
                    unused.remove(chosen["file"])
                except KeyError:
                    pass

    for target in targets:
        target_extras = (extras["*"] + extras[target.name]) or [""]
        for extra in target_extras:
            _verify_requirements(target, extra, [])

    unused = list(unused)
    unused.sort()
    return unmet, unused

def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="""Verifies that the requirements for the Python package distributions in an
            archive are satisfied by the Python package distributions in a list of other
            archives.""",
    )
    parser.add_argument(
        "-x",
        "--deps-extensions",
        action="append",
        default=["whl"],
        metavar="EXT",
        help="only treat files beneath DIR with extension EXT as distribution archives",
    )
    parser.add_argument(
        "-m",
        "--module-dir",
        metavar="DIR",
        help="search distribution archives for metadata beneath DIR as well as at the top level",
    )
    parser.add_argument(
        "-e",
        "--extra",
        action="append",
        default=[],
        metavar="EXTRA",
        help="additionally verify requirements for EXTRAs in TARGET's distributions",
    )
    parser.add_argument(
        "-u",
        "--unused",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="warn when DEPs do not satisfy any requirements for TARGET's distributions",
    )
    parser.add_argument(
        "target",
        metavar="TARGET",
        help="verify the requirements of the distributions in the archive at TARGET",
    )
    parser.add_argument(
        "deps_path",
        metavar="PATH",
        help="treat archives beneath PATH as dependencies of TARGET's distributions",
    )
    return parser

def main(args: argparse.Namespace):
    logging.basicConfig(
        format="%(asctime)s %(levelname)s: %(message)s",
        level=logging.DEBUG,
    )

    targets = get_dist_metadata(args.target)
    extras = defaultdict(list)
    for e in args.extra:
        pkg, _, extra = e.partition(".")
        pkg = str(canonicalize_name(pkg))
        if extra:
            extras[pkg].append(extra)
        else:
            extras["*"].append(extra)
    extensions_re = build_file_extension_re(args.deps_extensions)
    deps = defaultdict(list)
    for dep_path in find_dist_archives(args.deps_path, extensions_re, [args.target]):
        for dep in get_dist_metadata(dep_path, args.module_dir):
            name = str(canonicalize_name(dep.name))
            deps[name].append({"file": dep_path, "metadata": dep})

    unmet, unused = verify_requirements(targets, extras, deps)
    if unmet:
        for dep_stack, req, reason in unmet:
            logger.error(
                "Dependency verification failed: %s: %s: %s",
                " -> ".join([d.name for d in dep_stack]),
                str(req),
                reason,
            )
        return 1
    if args.unused and unused:
        for file in unused:
            print(f"unused_dep:{file}")

    return 0
    
if __name__ == "__main__":
    sys.exit(main(build_arg_parser().parse_args()))
