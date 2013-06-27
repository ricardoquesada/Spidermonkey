# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import unicode_literals

import os
import unittest

from mozunit import main

from mozbuild.frontend.data import (
    ConfigFileSubstitution,
    DirectoryTraversal,
    ReaderSummary,
    VariablePassthru,
)
from mozbuild.frontend.emitter import TreeMetadataEmitter
from mozbuild.frontend.reader import BuildReader

from mozbuild.test.common import MockConfig


data_path = os.path.abspath(os.path.dirname(__file__))
data_path = os.path.join(data_path, 'data')


class TestEmitterBasic(unittest.TestCase):
    def reader(self, name):
        config = MockConfig(os.path.join(data_path, name))
        config.substs['ENABLE_TESTS'] = '1'

        return BuildReader(config)

    def read_topsrcdir(self, reader):
        emitter = TreeMetadataEmitter(reader.config)

        objs = list(emitter.emit(reader.read_topsrcdir()))
        self.assertGreater(len(objs), 0)
        self.assertIsInstance(objs[-1], ReaderSummary)

        return objs[:-1]

    def test_dirs_traversal_simple(self):
        reader = self.reader('traversal-simple')
        objs = self.read_topsrcdir(reader)
        self.assertEqual(len(objs), 4)

        for o in objs:
            self.assertIsInstance(o, DirectoryTraversal)
            self.assertEqual(o.parallel_dirs, [])
            self.assertEqual(o.tool_dirs, [])
            self.assertEqual(o.test_dirs, [])
            self.assertEqual(o.test_tool_dirs, [])
            self.assertEqual(len(o.tier_dirs), 0)
            self.assertEqual(len(o.tier_static_dirs), 0)
            self.assertTrue(os.path.isabs(o.sandbox_main_path))
            self.assertEqual(len(o.sandbox_all_paths), 1)

        reldirs = [o.relativedir for o in objs]
        self.assertEqual(reldirs, ['', 'foo', 'foo/biz', 'bar'])

        dirs = [o.dirs for o in objs]
        self.assertEqual(dirs, [['foo', 'bar'], ['biz'], [], []])

    def test_traversal_all_vars(self):
        reader = self.reader('traversal-all-vars')
        objs = self.read_topsrcdir(reader)
        self.assertEqual(len(objs), 6)

        for o in objs:
            self.assertIsInstance(o, DirectoryTraversal)

        reldirs = set([o.relativedir for o in objs])
        self.assertEqual(reldirs, set(['', 'parallel', 'regular', 'test',
            'test_tool', 'tool']))

        for o in objs:
            reldir = o.relativedir

            if reldir == '':
                self.assertEqual(o.dirs, ['regular'])
                self.assertEqual(o.parallel_dirs, ['parallel'])
                self.assertEqual(o.test_dirs, ['test'])
                self.assertEqual(o.test_tool_dirs, ['test_tool'])
                self.assertEqual(o.tool_dirs, ['tool'])
                self.assertEqual(o.external_make_dirs, ['external_make'])
                self.assertEqual(o.parallel_external_make_dirs,
                    ['parallel_external_make'])

    def test_tier_simple(self):
        reader = self.reader('traversal-tier-simple')
        objs = self.read_topsrcdir(reader)
        self.assertEqual(len(objs), 4)

        reldirs = [o.relativedir for o in objs]
        self.assertEqual(reldirs, ['', 'foo', 'foo/biz', 'bar'])

    def test_config_file_substitution(self):
        reader = self.reader('config-file-substitution')
        objs = self.read_topsrcdir(reader)
        self.assertEqual(len(objs), 3)

        self.assertIsInstance(objs[0], DirectoryTraversal)
        self.assertIsInstance(objs[1], ConfigFileSubstitution)
        self.assertIsInstance(objs[2], ConfigFileSubstitution)

        topobjdir = os.path.abspath(reader.config.topobjdir)
        self.assertEqual(objs[1].relpath, 'foo')
        self.assertEqual(os.path.normpath(objs[1].output_path),
            os.path.normpath(os.path.join(topobjdir, 'foo')))
        self.assertEqual(os.path.normpath(objs[2].output_path),
            os.path.normpath(os.path.join(topobjdir, 'bar')))

    def test_variable_passthru(self):
        reader = self.reader('variable-passthru')
        objs = self.read_topsrcdir(reader)

        self.assertEqual(len(objs), 2)
        self.assertIsInstance(objs[0], DirectoryTraversal)
        self.assertIsInstance(objs[1], VariablePassthru)

        variables = objs[1].variables
        self.assertEqual(len(variables), 3)
        self.assertIn('XPIDLSRCS', variables)
        self.assertEqual(variables['XPIDLSRCS'],
            ['foo.idl', 'bar.idl', 'biz.idl'])
        self.assertIn('XPIDL_MODULE', variables)
        self.assertEqual(variables['XPIDL_MODULE'], 'module_name')
        self.assertIn('XPIDL_FLAGS', variables)
        self.assertEqual(variables['XPIDL_FLAGS'],
            ['-Idir1', '-Idir2', '-Idir3'])


if __name__ == '__main__':
    main()
