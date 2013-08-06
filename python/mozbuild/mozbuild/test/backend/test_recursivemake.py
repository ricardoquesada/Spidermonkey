# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import unicode_literals

import os
import time

from mozunit import main

from mozbuild.backend.configenvironment import ConfigEnvironment
from mozbuild.backend.recursivemake import RecursiveMakeBackend
from mozbuild.frontend.emitter import TreeMetadataEmitter
from mozbuild.frontend.reader import BuildReader

from mozbuild.test.backend.common import BackendTester


class TestRecursiveMakeBackend(BackendTester):
    def test_basic(self):
        """Ensure the RecursiveMakeBackend works without error."""
        env = self._consume('stub0', RecursiveMakeBackend)
        self.assertTrue(os.path.exists(os.path.join(env.topobjdir,
            'backend.RecursiveMakeBackend.built')))

    def test_output_files(self):
        """Ensure proper files are generated."""
        env = self._consume('stub0', RecursiveMakeBackend)

        expected = ['', 'dir1', 'dir2']

        for d in expected:
            out_makefile = os.path.join(env.topobjdir, d, 'Makefile')
            out_backend = os.path.join(env.topobjdir, d, 'backend.mk')

            self.assertTrue(os.path.exists(out_makefile))
            self.assertTrue(os.path.exists(out_backend))

    def test_makefile_conversion(self):
        """Ensure Makefile.in is converted properly."""
        env = self._consume('stub0', RecursiveMakeBackend)

        p = os.path.join(env.topobjdir, 'Makefile')

        lines = [l.strip() for l in open(p, 'rt').readlines()[3:]]
        self.assertEqual(lines, [
            'DEPTH := .',
            'topsrcdir := %s' % env.topsrcdir,
            'srcdir := %s' % env.topsrcdir,
            'VPATH = %s' % env.topsrcdir,
            '',
            'include $(DEPTH)/config/autoconf.mk',
            '',
            'include $(topsrcdir)/config/rules.mk'
        ])

    def test_missing_makefile_in(self):
        """Ensure missing Makefile.in results in Makefile creation."""
        env = self._consume('stub0', RecursiveMakeBackend)

        p = os.path.join(env.topobjdir, 'dir2', 'Makefile')
        self.assertTrue(os.path.exists(p))

        lines = [l.strip() for l in open(p, 'rt').readlines()]
        self.assertEqual(len(lines), 9)

        self.assertTrue(lines[0].startswith('# THIS FILE WAS AUTOMATICALLY'))

    def test_backend_mk(self):
        """Ensure backend.mk file is written out properly."""
        env = self._consume('stub0', RecursiveMakeBackend)

        p = os.path.join(env.topobjdir, 'backend.mk')

        lines = [l.strip() for l in open(p, 'rt').readlines()[2:-1]]
        self.assertEqual(lines, [
            'MOZBUILD_DERIVED := 1',
            'NO_MAKEFILE_RULE := 1',
            'NO_SUBMAKEFILES_RULE := 1',
            'DIRS := dir1',
            'PARALLEL_DIRS := dir2',
            'TEST_DIRS := dir3',
            'SUBSTITUTE_FILES += Makefile',
        ])

    def test_mtime_no_change(self):
        """Ensure mtime is not updated if file content does not change."""

        env = self._consume('stub0', RecursiveMakeBackend)

        makefile_path = os.path.join(env.topobjdir, 'Makefile')
        backend_path = os.path.join(env.topobjdir, 'backend.mk')
        makefile_mtime = os.path.getmtime(makefile_path)
        backend_mtime = os.path.getmtime(backend_path)

        reader = BuildReader(env)
        emitter = TreeMetadataEmitter(env)
        backend = RecursiveMakeBackend(env)
        backend.consume(emitter.emit(reader.read_topsrcdir()))

        self.assertEqual(os.path.getmtime(makefile_path), makefile_mtime)
        self.assertEqual(os.path.getmtime(backend_path), backend_mtime)

    def test_external_make_dirs(self):
        """Ensure we have make recursion into external make directories."""
        env = self._consume('external_make_dirs', RecursiveMakeBackend)

        backend_path = os.path.join(env.topobjdir, 'backend.mk')
        lines = [l.strip() for l in open(backend_path, 'rt').readlines()[2:-1]]
        self.assertEqual(lines, [
            'MOZBUILD_DERIVED := 1',
            'NO_MAKEFILE_RULE := 1',
            'NO_SUBMAKEFILES_RULE := 1',
            'DIRS := dir',
            'PARALLEL_DIRS := p_dir',
            'DIRS += external',
            'PARALLEL_DIRS += p_external',
            'SUBSTITUTE_FILES += Makefile',
        ])

    def test_substitute_config_files(self):
        """Ensure substituted config files are produced."""
        env = self._consume('substitute_config_files', RecursiveMakeBackend)

        p = os.path.join(env.topobjdir, 'foo')
        self.assertTrue(os.path.exists(p))
        lines = [l.strip() for l in open(p, 'rt').readlines()]
        self.assertEqual(lines, [
            'TEST = foo',
        ])

    def test_variable_passthru(self):
        """Ensure variable passthru is written out correctly."""
        env = self._consume('variable_passthru', RecursiveMakeBackend)

        backend_path = os.path.join(env.topobjdir, 'backend.mk')
        lines = [l.strip() for l in open(backend_path, 'rt').readlines()[2:-1]]
        self.assertEqual(lines[3:6], [
            'XPIDLSRCS += foo.idl',
            'XPIDLSRCS += bar.idl',
            'XPIDLSRCS += biz.idl',
        ])
        self.assertEqual(lines[6:9], [
            'XPIDL_FLAGS += -Idir1',
            'XPIDL_FLAGS += -Idir2',
            'XPIDL_FLAGS += -Idir3',
        ])
        self.assertEqual(lines[9], 'XPIDL_MODULE := module_name')

    def test_exports(self):
        """Ensure EXPORTS is written out correctly."""
        env = self._consume('exports', RecursiveMakeBackend)

        backend_path = os.path.join(env.topobjdir, 'backend.mk')
        lines = [l.strip() for l in open(backend_path, 'rt').readlines()[2:-1]]

        self.assertEqual(lines, [
            'MOZBUILD_DERIVED := 1',
            'NO_MAKEFILE_RULE := 1',
            'NO_SUBMAKEFILES_RULE := 1',
            'EXPORTS += foo.h',
            'EXPORTS_NAMESPACES += mozilla',
            'EXPORTS_mozilla += mozilla1.h mozilla2.h',
            'EXPORTS_NAMESPACES += mozilla/dom',
            'EXPORTS_mozilla/dom += dom1.h dom2.h',
            'EXPORTS_NAMESPACES += mozilla/gfx',
            'EXPORTS_mozilla/gfx += gfx.h',
            'EXPORTS_NAMESPACES += nspr/private',
            'EXPORTS_nspr/private += pprio.h',
        ])

    def test_xpcshell_manifests(self):
        """Ensure XPCSHELL_TESTS_MANIFESTS is written out correctly."""
        env = self._consume('xpcshell_manifests', RecursiveMakeBackend)

        backend_path = os.path.join(env.topobjdir, 'backend.mk')
        lines = [l.strip() for l in open(backend_path, 'rt').readlines()[2:-1]]

        # Avoid positional parameter and async related breakage
        var = 'XPCSHELL_TESTS'
        xpclines = sorted([val for val in lines if val.startswith(var)])

        # Assignment[aa], append[cc], conditional[valid]
        expected = ('aa', 'bb', 'cc', 'dd', 'valid_val')
        self.assertEqual(xpclines, ["XPCSHELL_TESTS += %s" % val for val in expected])

if __name__ == '__main__':
    main()
