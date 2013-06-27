# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This file contains code for reading metadata from the build system into
# data structures.

r"""Read build frontend files into data structures.

In terms of code architecture, the main interface is BuildReader. BuildReader
starts with a root mozbuild file. It creates a new execution environment for
this file, which is represented by the Sandbox class. The Sandbox class is what
defines what is allowed to execute in an individual mozbuild file. The Sandbox
consists of a local and global namespace, which are modeled by the
LocalNamespace and GlobalNamespace classes, respectively. The global namespace
contains all of the takeaway information from the execution. The local
namespace is for throwaway local variables and its contents are discarded after
execution.

The BuildReader contains basic logic for traversing a tree of mozbuild files.
It does this by examining specific variables populated during execution.
"""

from __future__ import print_function, unicode_literals

import logging
import os
import sys
import time
import traceback
import types

from io import StringIO

from mozbuild.util import (
    ReadOnlyDefaultDict,
    ReadOnlyDict,
)

from mozbuild.backend.configenvironment import ConfigEnvironment

from .sandbox import (
    SandboxError,
    SandboxExecutionError,
    SandboxLoadError,
    Sandbox,
)

from .sandbox_symbols import (
    FUNCTIONS,
    VARIABLES,
)


if sys.version_info.major == 2:
    text_type = unicode
    type_type = types.TypeType
else:
    text_type = str
    type_type = type


def log(logger, level, action, params, formatter):
    logger.log(level, formatter, extra={'action': action, 'params': params})


def is_read_allowed(path, config):
    """Whether we are allowed to load a mozbuild file at the specified path.

    This is used as cheap security to ensure the build is isolated to known
    source directories.

    We are allowed to read from the main source directory and any defined
    external source directories. The latter is to allow 3rd party applications
    to hook into our build system.
    """
    assert os.path.isabs(path)
    assert os.path.isabs(config.topsrcdir)

    path = os.path.normpath(path)
    topsrcdir = os.path.normpath(config.topsrcdir)

    if path.startswith(topsrcdir):
        return True

    external_dirs = config.substs.get('EXTERNAL_SOURCE_DIR', '').split()
    for external in external_dirs:
        if not os.path.isabs(external):
            external = os.path.join(config.topsrcdir, external)
        external = os.path.normpath(external)

        if path.startswith(external):
            return True

    return False


class SandboxCalledError(SandboxError):
    """Represents an error resulting from calling the error() function."""

    def __init__(self, file_stack, message):
        SandboxError.__init__(self, file_stack)
        self.message = message


class MozbuildSandbox(Sandbox):
    """Implementation of a Sandbox tailored for mozbuild files.

    We expose a few useful functions and expose the set of variables defining
    Mozilla's build system.
    """
    def __init__(self, config, path):
        """Create an empty mozbuild Sandbox.

        config is a ConfigStatus instance (the output of configure). path is
        the path of the main mozbuild file that is being executed. It is used
        to compute encountered relative paths.
        """
        Sandbox.__init__(self, allowed_variables=VARIABLES)

        self._log = logging.getLogger(__name__)

        self.config = config

        topobjdir = os.path.abspath(config.topobjdir)
        topsrcdir = config.topsrcdir
        norm_topsrcdir = os.path.normpath(topsrcdir)

        if not path.startswith(norm_topsrcdir):
            external_dirs = config.substs.get('EXTERNAL_SOURCE_DIR', '').split()
            for external in external_dirs:
                external = os.path.normpath(external)

                if not os.path.isabs(external):
                    external = os.path.join(config.topsrcdir, external)

                external = os.path.normpath(external)

                if not path.startswith(external):
                    continue

                topsrcdir = external

                # This is really hacky and should be replaced with something
                # more robust. We assume that if an external source directory
                # is in play that the main build system is built in a
                # subdirectory of its topobjdir. Therefore, the topobjdir of
                # the external source directory is the parent of our topobjdir.
                topobjdir = os.path.dirname(topobjdir)

                # This is suboptimal because we load the config.status multiple
                # times. We should consider caching it, possibly by moving this
                # code up to the reader.
                config = ConfigEnvironment.from_config_status(
                    os.path.join(topobjdir, 'config.status'))
                self.config = config
                break

        self.topsrcdir = topsrcdir

        relpath = os.path.relpath(path, topsrcdir).replace(os.sep, '/')
        reldir = os.path.dirname(relpath)

        with self._globals.allow_all_writes() as d:
            d['TOPSRCDIR'] = topsrcdir
            d['TOPOBJDIR'] = topobjdir
            d['RELATIVEDIR'] = reldir
            d['SRCDIR'] = os.path.join(topsrcdir, reldir).replace(os.sep, '/').rstrip('/')
            d['OBJDIR'] = os.path.join(topobjdir, reldir).replace(os.sep, '/').rstrip('/')

            # config.status does not yet use unicode. However, mozbuild expects
            # unicode everywhere. So, decode binary into unicode as necessary.
            # Bug 844509 tracks a better way to do this.
            substs = {}
            for k, v in config.substs.items():
                if not isinstance(v, text_type):
                    try:
                        v = v.decode('utf-8')
                    except UnicodeDecodeError:
                        log(self._log, logging.INFO, 'lossy_encoding',
                            {'variable': k},
                            'Lossy Unicode encoding for {variable}. See bug 844509.')

                        v = v.decode('utf-8', 'replace')

                substs[k] = v

            d['CONFIG'] = ReadOnlyDefaultDict(substs, global_default=None)

            # Register functions.
            for name, func in FUNCTIONS.items():
                d[name] = getattr(self, func[0])

    def exec_file(self, path, filesystem_absolute=False):
        """Override exec_file to normalize paths and restrict file loading.

        If the path is absolute, behavior is governed by filesystem_absolute.
        If filesystem_absolute is True, the path is interpreted as absolute on
        the actual filesystem. If it is false, the path is treated as absolute
        within the current topsrcdir.

        If the path is not absolute, it will be treated as relative to the
        currently executing file. If there is no currently executing file, it
        will be treated as relative to topsrcdir.

        Paths will be rejected if they do not fall under topsrcdir.
        """
        if os.path.isabs(path):
            if not filesystem_absolute:
                path = os.path.normpath(os.path.join(self.topsrcdir,
                    path[1:]))

        else:
            if len(self._execution_stack):
                path = os.path.normpath(os.path.join(
                    os.path.dirname(self._execution_stack[-1]),
                    path))
            else:
                path = os.path.normpath(os.path.join(
                    self.topsrcdir, path))

        # realpath() is needed for true security. But, this isn't for security
        # protection, so it is omitted.
        normalized_path = os.path.normpath(path)
        if not is_read_allowed(normalized_path, self.config):
            raise SandboxLoadError(list(self._execution_stack),
                sys.exc_info()[2], illegal_path=path)

        Sandbox.exec_file(self, path)

    def _add_tier_directory(self, tier, reldir, static=False):
        """Register a tier directory with the build."""
        if isinstance(reldir, text_type):
            reldir = [reldir]

        if not tier in self['TIERS']:
            self['TIERS'][tier] = {
                'regular': [],
                'static': [],
            }

        key = 'static' if static else 'regular'

        for path in reldir:
            if path in self['TIERS'][tier][key]:
                raise Exception('Directory has already been registered with '
                    'tier: %s' % path)

            self['TIERS'][tier][key].append(path)

    def _include(self, path):
        """Include and exec another file within the context of this one."""

        # exec_file() handles normalization and verification of the path.
        self.exec_file(path)

    def _warning(self, message):
        # FUTURE consider capturing warnings in a variable instead of printing.
        print('WARNING: %s' % message, file=sys.stderr)

    def _error(self, message):
        raise SandboxCalledError(self._execution_stack, message)


class SandboxValidationError(Exception):
    """Represents an error encountered when validating sandbox results."""
    pass


class BuildReaderError(Exception):
    """Represents errors encountered during BuildReader execution.

    The main purpose of this class is to facilitate user-actionable error
    messages. Execution errors should say:

      - Why they failed
      - Where they failed
      - What can be done to prevent the error

    A lot of the code in this class should arguably be inside sandbox.py.
    However, extraction is somewhat difficult given the additions
    MozbuildSandbox has over Sandbox (e.g. the concept of included files -
    which affect error messages, of course).
    """
    def __init__(self, file_stack, trace, sandbox_exec_error=None,
        sandbox_load_error=None, validation_error=None, other_error=None,
        sandbox_called_error=None):

        self.file_stack = file_stack
        self.trace = trace
        self.sandbox_called_error = sandbox_called_error
        self.sandbox_exec = sandbox_exec_error
        self.sandbox_load = sandbox_load_error
        self.validation_error = validation_error
        self.other = other_error

    @property
    def main_file(self):
        return self.file_stack[-1]

    @property
    def actual_file(self):
        # We report the file that called out to the file that couldn't load.
        if self.sandbox_load is not None:
            if len(self.sandbox_load.file_stack) > 1:
                return self.sandbox_load.file_stack[-2]

            if len(self.file_stack) > 1:
                return self.file_stack[-2]

        if self.sandbox_error is not None and \
            len(self.sandbox_error.file_stack):
            return self.sandbox_error.file_stack[-1]

        return self.file_stack[-1]

    @property
    def sandbox_error(self):
        return self.sandbox_exec or self.sandbox_load or \
            self.sandbox_called_error

    def __str__(self):
        s = StringIO()

        delim = '=' * 30
        s.write('%s\nERROR PROCESSING MOZBUILD FILE\n%s\n\n' % (delim, delim))

        s.write('The error occurred while processing the following file:\n')
        s.write('\n')
        s.write('    %s\n' % self.actual_file)
        s.write('\n')

        if self.actual_file != self.main_file and not self.sandbox_load:
            s.write('This file was included as part of processing:\n')
            s.write('\n')
            s.write('    %s\n' % self.main_file)
            s.write('\n')

        if self.sandbox_error is not None:
            self._print_sandbox_error(s)
        elif self.validation_error is not None:
            s.write('The error occurred when validating the result of ')
            s.write('the execution. The reported error is:\n')
            s.write('\n')
            s.write('    %s\n' % self.validation_error.message)
            s.write('\n')
        else:
            s.write('The error appears to be part of the %s ' % __name__)
            s.write('Python module itself! It is possible you have stumbled ')
            s.write('across a legitimate bug.\n')
            s.write('\n')

            for l in traceback.format_exception(type(self.other), self.other,
                self.trace):
                s.write(unicode(l))

        return s.getvalue()

    def _print_sandbox_error(self, s):
        # Try to find the frame of the executed code.
        script_frame = None

        # We don't currently capture the trace for SandboxCalledError.
        # Therefore, we don't get line numbers from the moz.build file.
        # FUTURE capture this.
        trace = getattr(self.sandbox_error, 'trace', None)
        frames = []
        if trace:
            frames = traceback.extract_tb(trace)
        for frame in frames:
            if frame[0] == self.actual_file:
                script_frame = frame

            # Reset if we enter a new execution context. This prevents errors
            # in this module from being attributes to a script.
            elif frame[0] == __file__ and frame[2] == 'exec_source':
                script_frame = None

        if script_frame is not None:
            s.write('The error was triggered on line %d ' % script_frame[1])
            s.write('of this file:\n')
            s.write('\n')
            s.write('    %s\n' % script_frame[3])
            s.write('\n')

        if self.sandbox_called_error is not None:
            self._print_sandbox_called_error(s)
            return

        if self.sandbox_load is not None:
            self._print_sandbox_load_error(s)
            return

        self._print_sandbox_exec_error(s)

    def _print_sandbox_called_error(self, s):
        assert self.sandbox_called_error is not None

        s.write('A moz.build file called the error() function.\n')
        s.write('\n')
        s.write('The error it encountered is:\n')
        s.write('\n')
        s.write('    %s\n' % self.sandbox_called_error.message)
        s.write('\n')
        s.write('Correct the error condition and try again.\n')

    def _print_sandbox_load_error(self, s):
        assert self.sandbox_load is not None

        if self.sandbox_load.illegal_path is not None:
            s.write('The underlying problem is an illegal file access. ')
            s.write('This is likely due to trying to access a file ')
            s.write('outside of the top source directory.\n')
            s.write('\n')
            s.write('The path whose access was denied is:\n')
            s.write('\n')
            s.write('    %s\n' % self.sandbox_load.illegal_path)
            s.write('\n')
            s.write('Modify the script to not access this file and ')
            s.write('try again.\n')
            return

        if self.sandbox_load.read_error is not None:
            if not os.path.exists(self.sandbox_load.read_error):
                s.write('The underlying problem is we referenced a path ')
                s.write('that does not exist. That path is:\n')
                s.write('\n')
                s.write('    %s\n' % self.sandbox_load.read_error)
                s.write('\n')
                s.write('Either create the file if it needs to exist or ')
                s.write('do not reference it.\n')
            else:
                s.write('The underlying problem is a referenced path could ')
                s.write('not be read. The trouble path is:\n')
                s.write('\n')
                s.write('    %s\n' % self.sandbox_load.read_error)
                s.write('\n')
                s.write('It is possible the path is not correct. Is it ')
                s.write('pointing to a directory? It could also be a file ')
                s.write('permissions issue. Ensure that the file is ')
                s.write('readable.\n')

            return

        # This module is buggy if you see this.
        raise AssertionError('SandboxLoadError with unhandled properties!')

    def _print_sandbox_exec_error(self, s):
        assert self.sandbox_exec is not None

        inner = self.sandbox_exec.exc_value

        if isinstance(inner, SyntaxError):
            s.write('The underlying problem is a Python syntax error ')
            s.write('on line %d:\n' % inner.lineno)
            s.write('\n')
            s.write('    %s\n' % inner.text)
            s.write((' ' * (inner.offset + 4)) + '^\n')
            s.write('\n')
            s.write('Fix the syntax error and try again.\n')
            return

        if isinstance(inner, KeyError):
            self._print_keyerror(inner, s)
        elif isinstance(inner, ValueError):
            self._print_valueerror(inner, s)
        else:
            self._print_exception(inner, s)

    def _print_keyerror(self, inner, s):
        if inner.args[0] not in ('global_ns', 'local_ns'):
            self._print_exception(unner, s)
            return

        if inner.args[0] == 'global_ns':
            verb = None
            if inner.args[1] == 'get_unknown':
                verb = 'read'
            elif inner.args[1] == 'set_unknown':
                verb = 'write'
            else:
                raise AssertionError('Unhandled global_ns: %s' % inner.args[1])

            s.write('The underlying problem is an attempt to %s ' % verb)
            s.write('a reserved UPPERCASE variable that does not exist.\n')
            s.write('\n')
            s.write('The variable %s causing the error is:\n' % verb)
            s.write('\n')
            s.write('    %s\n' % inner.args[2])
            s.write('\n')
            s.write('Please change the file to not use this variable.\n')
            s.write('\n')
            s.write('For reference, the set of valid variables is:\n')
            s.write('\n')
            s.write(', '.join(sorted(VARIABLES.keys())) + '\n')
            return

        s.write('The underlying problem is a reference to an undefined ')
        s.write('local variable:\n')
        s.write('\n')
        s.write('    %s\n' % inner.args[2])
        s.write('\n')
        s.write('Please change the file to not reference undefined ')
        s.write('variables and try again.\n')

    def _print_valueerror(self, inner, s):
        if inner.args[0] not in ('global_ns', 'local_ns'):
            self._print_exception(inner, s)
            return

        assert inner.args[1] == 'set_type'

        s.write('The underlying problem is an attempt to write an illegal ')
        s.write('value to a special variable.\n')
        s.write('\n')
        s.write('The variable whose value was rejected is:\n')
        s.write('\n')
        s.write('    %s' % inner.args[2])
        s.write('\n')
        s.write('The value being written to it was of the following type:\n')
        s.write('\n')
        s.write('    %s\n' % type(inner.args[3]).__name__)
        s.write('\n')
        s.write('This variable expects the following type(s):\n')
        s.write('\n')
        if type(inner.args[4]) == type_type:
            s.write('    %s\n' % inner.args[4].__name__)
        else:
            for t in inner.args[4]:
                s.write( '    %s\n' % t.__name__)
        s.write('\n')
        s.write('Change the file to write a value of the appropriate type ')
        s.write('and try again.\n')

    def _print_exception(self, e, s):
        s.write('An error was encountered as part of executing the file ')
        s.write('itself. The error appears to be the fault of the script.\n')
        s.write('\n')
        s.write('The error as reported by Python is:\n')
        s.write('\n')
        s.write('    %s\n' % traceback.format_exception_only(type(e), e))


class BuildReader(object):
    """Read a tree of mozbuild files into data structures.

    This is where the build system starts. You give it a tree configuration
    (the output of configuration) and it executes the moz.build files and
    collects the data they define.
    """

    def __init__(self, config):
        self.config = config
        self.topsrcdir = config.topsrcdir

        self._log = logging.getLogger(__name__)
        self._read_files = set()
        self._execution_stack = []

    def read_topsrcdir(self):
        """Read the tree of mozconfig files into a data structure.

        This starts with the tree's top-most mozbuild file and descends into
        all linked mozbuild files until all relevant files have been evaluated.

        This is a generator of Sandbox instances. As each mozbuild file is
        read, a new Sandbox is created. Each created Sandbox is returned.
        """
        path = os.path.join(self.topsrcdir, 'moz.build')
        return self.read_mozbuild(path, read_tiers=True,
            filesystem_absolute=True)

    def read_mozbuild(self, path, read_tiers=False, filesystem_absolute=False,
            descend=True):
        """Read and process a mozbuild file, descending into children.

        This starts with a single mozbuild file, executes it, and descends into
        other referenced files per our traversal logic.

        The traversal logic is to iterate over the *DIRS variables, treating
        each element as a relative directory path. For each encountered
        directory, we will open the moz.build file located in that
        directory in a new Sandbox and process it.

        If read_tiers is True (it should only be True for the top-level
        mozbuild file in a project), the TIERS variable will be used for
        traversal as well.

        If descend is True (the default), we will descend into child
        directories and files per variable values.

        Traversal is performed depth first (for no particular reason).
        """
        self._execution_stack.append(path)
        try:
            for s in self._read_mozbuild(path, read_tiers=read_tiers,
                filesystem_absolute=filesystem_absolute, descend=descend):
                yield s

        except BuildReaderError as bre:
            raise bre

        except SandboxCalledError as sce:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], sandbox_called_error=sce)

        except SandboxExecutionError as se:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], sandbox_exec_error=se)

        except SandboxLoadError as sle:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], sandbox_load_error=sle)

        except SandboxValidationError as ve:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], validation_error=ve)

        except Exception as e:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], other_error=e)

    def _read_mozbuild(self, path, read_tiers, filesystem_absolute, descend):
        path = os.path.normpath(path)
        log(self._log, logging.DEBUG, 'read_mozbuild', {'path': path},
            'Reading file: {path}')

        if path in self._read_files:
            log(self._log, logging.WARNING, 'read_already', {'path': path},
                'File already read. Skipping: {path}')
            return

        self._read_files.add(path)

        time_start = time.time()
        sandbox = MozbuildSandbox(self.config, path)
        sandbox.exec_file(path, filesystem_absolute=filesystem_absolute)
        sandbox.execution_time = time.time() - time_start
        yield sandbox

        # Traverse into referenced files.

        # We first collect directories populated in variables.
        dir_vars = ['DIRS', 'PARALLEL_DIRS', 'TOOL_DIRS']

        if self.config.substs.get('ENABLE_TESTS', False) == '1':
            dir_vars.extend(['TEST_DIRS', 'TEST_TOOL_DIRS'])

        # It's very tempting to use a set here. Unfortunately, the recursive
        # make backend needs order preserved. Once we autogenerate all backend
        # files, we should be able to convert this to a set.
        dirs = []
        for var in dir_vars:
            if not var in sandbox:
                continue

            for d in sandbox[var]:
                if d in dirs:
                    raise SandboxValidationError(
                        'Directory (%s) registered multiple times in %s' % (
                            d, var))

                dirs.append(d)

        # We also have tiers whose members are directories.
        if 'TIERS' in sandbox:
            if not read_tiers:
                raise SandboxValidationError(
                    'TIERS defined but it should not be')

            for tier, values in sandbox['TIERS'].items():
                # We don't descend into static directories because static by
                # definition is external to the build system.
                for d in values['regular']:
                    if d in dirs:
                        raise SandboxValidationError(
                            'Tier directory (%s) registered multiple '
                            'times in %s' % (d, tier))
                    dirs.append(d)

        curdir = os.path.dirname(path)
        for relpath in dirs:
            child_path = os.path.join(curdir, relpath, 'moz.build')

            # Ensure we don't break out of the topsrcdir. We don't do realpath
            # because it isn't necessary. If there are symlinks in the srcdir,
            # that's not our problem. We're not a hosted application: we don't
            # need to worry about security too much.
            child_path = os.path.normpath(child_path)
            if not is_read_allowed(child_path, self.config):
                raise SandboxValidationError(
                    'Attempting to process file outside of allowed paths: %s' %
                        child_path)

            if not descend:
                continue

            for res in self.read_mozbuild(child_path, read_tiers=False,
                filesystem_absolute=True):
                yield res

        self._execution_stack.pop()

