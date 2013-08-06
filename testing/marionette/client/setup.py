import os
from setuptools import setup, find_packages

version = '0.5.27'

# get documentation from the README
try:
    here = os.path.dirname(os.path.abspath(__file__))
    description = file(os.path.join(here, 'README.md')).read()
except (OSError, IOError):
    description = ''

# dependencies
deps = ['manifestdestiny', 'mozhttpd >= 0.5',
        'mozprocess >= 0.9', 'mozrunner >= 5.15',
        'mozdevice >= 0.22', 'moznetwork >= 0.21',
        'mozcrash >= 0.5', 'mozprofile >= 0.7']

setup(name='marionette_client',
      version=version,
      description="Marionette test automation client",
      long_description=description,
      classifiers=[], # Get strings from http://pypi.python.org/pypi?%3Aaction=list_classifiers
      keywords='mozilla',
      author='Jonathan Griffin',
      author_email='jgriffin@mozilla.com',
      url='https://wiki.mozilla.org/Auto-tools/Projects/Marionette',
      license='MPL',
      packages=find_packages(exclude=['ez_setup', 'examples', 'tests']),
      package_data={'marionette': ['touch/*.js']},
      include_package_data=True,
      zip_safe=False,
      entry_points="""
      # -*- Entry points: -*-
      [console_scripts]
      marionette = marionette.runtests:cli
      """,
      install_requires=deps,
      )

