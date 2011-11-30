# Copyright (c) 2011 The Native Client Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Class that represents an NSIS installer script."""

import os
import subprocess
import tarfile
import tempfile

from build_tools import path_set


class Error(Exception):
  pass


class NsisScript(path_set.PathSet):
  '''Container for a NSIS script file

  Use this class to create and manage an NSIS script.  You can construct this
  class with an existing script file.  You can Compile() the script to produce
  and NSIS installer.
  '''

  def __init__(self, script_file='nsis.nsi'):
    path_set.PathSet.__init__(self)
    self._script_file = script_file
    self._install_dir = os.path.join('C:%s' % os.sep, 'nsis_install')
    self._relative_install_root = None

  @property
  def script_file(self):
    '''The output NSIS script file. Read-only.'''
    return self._script_file

  @property
  def install_dir(self):
    '''The default directory where the NSIS installer will unpack its contents.

    This must be a full path, including the dirve letter.  E.g.:
        C:\native_client_sdk
    '''
    return self._install_dir

  @install_dir.setter
  def install_dir(self, new_install_dir):
    if (os.path.isabs(new_install_dir) and
        len(os.path.splitdrive(new_install_dir)[0]) > 0):
      self._install_dir = new_install_dir
    else:
      raise Error('install_dir must be an absolute path')

  def InitFromDirectory(self,
                        artifact_dir,
                        dir_filter=None,
                        file_filter=None):
    '''Create the list of installer artifacts.

    Creates three lists:
      1. a list of directories that need to be generated by the installer
      2. a list of files that get installed into those directories
      3. a list of symbolic links that need to be created by the installer
    These lists are used later on when generating the section commands that are
    compiled into the final installer

    Args:
      artifact_dir: The directory containing the files to add to the NSIS
          installer script.  The NSIS installer will reproduce this directory
          structure.

      dir_filter: A filter function for directories.  This can be written as a
          list comprehension.  If dir_filter is not None, then it is called
          with |_dirs|, and |_dirs| is replaced with the filter's
          output.

      file_filter: A filter function for files.  This can be written as a
          list comprehension.  If file_filter is not None, then it is called
          with |_files|, and |_files| is replaced with the filter's
          output.
    '''
    self._relative_install_root = artifact_dir
    self.Reset()
    for root, dirs, files in os.walk(artifact_dir):
      map(lambda dir: self._dirs.add(os.path.join(root, dir)), dirs)
      map(lambda file: self._files.add(os.path.join(root, file)), files)
    if dir_filter:
      self._dirs = set(dir_filter(self._dirs))
    if file_filter:
      self._files = set(file_filter(self._files))

    def ReadCygwinSymlink(path):
      if not os.path.isfile(path):
        return None
      header = open(path, 'rb').read(12)
      if header != '!<symlink>\xff\xfe':
        return None
      data = open(path, 'rb').read()[12:]
      return ''.join([ch for ch in data if ch != '\x00'])

    # Pick out cygwin symlinks.
    all_files = self._files
    self._files = set()
    for f in all_files:
      link = ReadCygwinSymlink(f)
      if link:
        self._symlinks[f] = link
      else:
        self._files.add(f)

  def CreateInstallNameScript(self, cwd='.'):
    '''Write out the installer name script.

    The installer name script is in the special NSIS include file
    sdk_install_name.nsh.  This file is expected to be in |cwd|.  If
    sdk_install_name.nsh already exists, it is overwritten.

    Args:
      cwd: The directory where sdk_install_name.sdk is placed.
    '''
    with open(os.path.join(cwd, 'sdk_install_name.nsh'), 'wb') as script:
      script.write('InstallDir %s\n' % self._install_dir)

  def NormalizeInstallPath(self, path):
    '''Normalize |path| for installation.

    If |_relative_install_root| is set, then normalize |path| by making it
    relative to |_relative_install_root|.

    Args:
      path: The path to normalize.

    Returns: the normalized path.  If |_relative_install_root| is None, then
        the return value is the same as |path|.
    '''
    if (self._relative_install_root and
        path.startswith(self._relative_install_root)):
      return path[len(self._relative_install_root) + 1:]
    else:
      return path

  def CreateSectionNameScript(self, cwd='.'):
    '''Write out the section script.

    The section script is in the special NSIS include file sdk_section.nsh.
    This file is expected to be in |cwd|.  If sdk_section.nsh already exists,
    it is overwritten.

    If |_relative_install_root| is set, then that part of the path is stripped
    off of the installed file and directory names.  This will cause the files
    to be installed as if they were relative to |_relative_install_root| and
    not in an absolute location.

    Args:
      cwd: The directory where sdk_section.sdk is placed.
    '''

    def SymlinkType(symlink):
      '''Return whether the source of symlink is a file or a directory.'''
      symlink_basename = os.path.basename(symlink)
      for file in self._files:
        if os.path.basename(file) == symlink_basename:
          return 'SoftF'
      return 'SoftD'

    with open(os.path.join(cwd, 'sdk_section.nsh'), 'wb') as script:
      script.write('Section "!Native Client SDK" NativeClientSDK\n')
      script.write('  SectionIn RO\n')
      script.write('  SetOutPath $INSTDIR\n')
      for dir in self._dirs:
        dir = self.NormalizeInstallPath(dir)
        script.write('  CreateDirectory "%s"\n' % os.path.join('$INSTDIR', dir))
      for file in self._files:
        file_norm = self.NormalizeInstallPath(file)
        script.write('  File "/oname=%s" "%s"\n' % (file_norm, file))
      for src, symlink in self._symlinks.items():
        src_norm = self.NormalizeInstallPath(src)
        link_type = SymlinkType(symlink)
        script.write('  MkLink::%s "%s" "%s"\n' % (
            link_type, os.path.join('$INSTDIR', src_norm), symlink))
      for src, link in self._links.items():
        src_norm = self.NormalizeInstallPath(src)
        link_norm = self.NormalizeInstallPath(link)
        script.write('  MkLink::Hard "%s" "%s"\n' % (
            os.path.join('$INSTDIR', src_norm),
            os.path.join('$INSTDIR', link_norm)))
      script.write('SectionEnd\n')

  def Compile(self):
    '''Compile the script.

    Compilation happens in a couple of steps: first, the install directory
    script is generated from |_install_dir|, then the section commands script
    is generated from |_files|.  Finally |_script_file| is compiled, which
    produces the NSIS installer specified by the OutFile property in
    |_script_file|.
    '''
    working_dir = os.path.dirname(self._script_file)
    self.CreateInstallNameScript(cwd=working_dir)
    self.CreateSectionNameScript(cwd=working_dir)
    # Run the NSIS compiler.
    subprocess.check_call([os.path.join('NSIS', 'makensis'),
                           '/V2',
                           self._script_file],
                          cwd=working_dir,
                          shell=True)
