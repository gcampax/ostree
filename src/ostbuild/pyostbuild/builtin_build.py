# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

import os,sys,subprocess,tempfile,re,shutil
import argparse
import json

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync
from . import ostbuildrc
from . import buildutil
from . import kvfile

class OstbuildBuild(builtins.Builtin):
    name = "build"
    short_description = "Rebuild all artifacts from the given manifest"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _ensure_vcs_checkout(self, name, keytype, uri, branch):
        assert keytype == 'git'
        destname = os.path.join(self.srcdir, name)
        tmp_destname = destname + '.tmp'
        if os.path.isdir(tmp_destname):
            shutil.rmtree(tmp_destname)
        if not os.path.isdir(destname):
            run_sync(['git', 'clone', uri, tmp_destname])
            os.rename(tmp_destname, destname)
        subprocess.check_call(['git', 'checkout', '-q', branch], cwd=destname)
        return destname

    def _get_vcs_version_from_checkout(self, name):
        vcsdir = os.path.join(self.srcdir, name)
        return subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=vcsdir)

    def _parse_src_key(self, srckey):
        idx = srckey.find(':')
        if idx < 0:
            raise ValueError("Invalid SRC uri=%s" % (srckey, ))
        keytype = srckey[:idx]
        if keytype not in ('git'):
            raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
        uri = srckey[idx+1:]
        idx = uri.rfind('#')
        if idx < 0:
            branch = "master"
        else:
            branch = uri[idx+1:]
            uri = uri[0:idx]
        return (keytype, uri, branch)

    def _parse_artifact_vcs_version(self, ver):
        idx = ver.rfind('-')
        if idx > 0:
            vcs_ver = ver[idx+1:]
        else:
            vcs_ver = ver
        if not vcs_ver.startswith('g'):
            raise ValueError("Invalid artifact version '%s'" % (ver, ))
        return vcs_ver[1:]

    def _build_one_component(self, name, architecture, meta):
        (keytype, uri, branch) = self._parse_src_key(meta['SRC'])
        component_src = self._ensure_vcs_checkout(name, keytype, uri, branch)
        buildroot = '%s-%s-devel' % (self.manifest['name'], architecture)
        branchname = 'artifacts/%s/%s/%s' % (buildroot, name, branch)
        try:
            previous_commit_version = subprocess.check_output(['ostree', '--repo=' + self.repo,
                                                               'rev-parse', branchname],
                                                              stderr=open('/dev/null', 'w'))
            previous_commit_version = previous_commit_version.strip()
            log("Previous build of '%s' is %s" % (branchname, previous_commit_version))
        except subprocess.CalledProcessError, e:
            previous_commit_version = None
            log("No previous build for '%s' found" % (branchname, ))
        if previous_commit_version is not None:
            previous_artifact_version = subprocess.check_output(['ostree', '--repo=' + self.repo,
                                                                 'show', '--print-metadata-key=ostbuild-artifact-version', previous_commit_version])
            previous_artifact_version = previous_artifact_version.strip()
            previous_buildroot_version = subprocess.check_output(['ostree', '--repo=' + self.repo,
                                                                  'show', '--print-metadata-key=ostbuild-buildroot-version', previous_commit_version])
            previous_buildroot_version = previous_buildroot_version.strip()
            current_buildroot_version = subprocess.check_output(['ostree', '--repo=' + self.repo,
                                                                 'rev-parse', buildroot])
            current_buildroot_version = current_buildroot_version.strip()
            
            previous_vcs_version = self._parse_artifact_vcs_version(previous_artifact_version)
            current_vcs_version = self._get_vcs_version_from_checkout(name)
            vcs_version_matches = False
            if previous_vcs_version == current_vcs_version:
                vcs_version_matches = True
                log("VCS version is unchanged from '%s'" % (previous_vcs_version, ))
            else:
                log("VCS version is now '%s', was '%s'" % (current_vcs_version, previous_vcs_version))
            buildroot_version_matches = False
            if vcs_version_matches:    
                buildroot_version_matches = (current_buildroot_version == previous_buildroot_version)
                if buildroot_version_matches:
                    log("Already have build '%s' of src commit '%s' for '%s' in buildroot '%s'" % (previous_commit_version, previous_vcs_version, branchname, buildroot))
                    return
                else:
                    log("Buildroot is now '%s'" % (current_buildroot_version, ))
        
        component_resultdir = os.path.join(self.workdir, 'name', 'results')
        if os.path.isdir(component_resultdir):
            shutil.rmtree(component_resultdir)
        os.makedirs(component_resultdir)
        current_machine = os.uname()[4]
        if current_machine != architecture:
            log("Current architecture '%s' differs from target '%s', using setarch" % (current_machine, architecture))
            args = ['setarch', architecture]
        else:
            args = []
        args.extend(['ostbuild', 'chroot-compile-one',
                     '--repo=' + self.repo,
                     '--buildroot=' + buildroot,
                     '--workdir=' + self.workdir,
                     '--resultdir=' + component_resultdir])
        run_sync(args, cwd=component_src)
        artifact_files = []
        for name in os.listdir(component_resultdir):
            if name.startswith('artifact-'):
                artifact_files.append(os.path.join(component_resultdir, name))
        assert len(artifact_files) >= 1 and len(artifact_files) <= 2
        run_sync(['ostbuild', 'commit-artifacts',
                  '--repo=' + self.repo] + artifact_files)
        artifacts = []
        for filename in artifact_files:
            parsed = buildutil.parse_artifact_name(name)
            artifacts.append(parsed)
        def _sort_artifact(a, b):
            if a['type'] == b['type']:
                return 0
            elif a['type'] == 'runtime':
                return -1
            return 1
        artifacts.sort(_sort_artifact)
        return artifacts

    def _compose(self, suffix, artifacts):
        compose_contents = [self.manifest['base'] + '-' + suffix]
        compose_contents.extend(artifacts)
        child_args = ['ostree', '--repo=' + self.repo, 'compose',
                      '-b', self.manifest['name'] + '-' + suffix, '-s', 'Compose']
        child_args.extend(compose_contents)
        run_sync(child_args)
    
    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--manifest', required=True)

        args = parser.parse_args(argv)

        self.parse_config()

        self.manifest = json.load(open(args.manifest))
        dirname = os.path.dirname(args.manifest)
        components = self.manifest['components']
        runtime_components = []
        devel_components = []
        runtime_artifacts = []
        devel_artifacts = []
        for component_name in components:
            for architecture in self.manifest['architectures']:
                path = os.path.join(dirname, component_name + '.txt')
                f = open(path)
                component_meta = kvfile.parse(f)
    
                artifact_branches = self._build_one_component(component_name, architecture, component_meta)
    
                target_component = component_meta.get('COMPONENT')
                if target_component == 'devel':
                    devel_components.append(component_name)
                else:
                    runtime_components.append(component_name)
                    for branch in artifact_branches:
                        if branch['type'] == 'runtime':
                            runtime_artifacts.append(branch)
                devel_artifacts.extend(artifact_branches)

                f.close()

                devel_branches = map(buildutil.branch_name_for_artifact, devel_artifacts)
                self._compose(architecture + '-devel', devel_branches)
                runtime_branches = map(buildutil.branch_name_for_artifact, runtime_artifacts)
                self._compose(architecture + '-runtime', runtime_artifacts)
        
builtins.register(OstbuildBuild)
