Dependencies
============

As Mesa is a core component of most Linux and BSD distributions and is required
for running a modern desktop environment, we have to be very careful when
choosing dependencies. Patches adding dependencies require special
consideration to ensure that they do not unnecessarily burden downstream
distributions.

Sometimes we have in-tree experiments where distributions and users are
encouraged to not use them by default. For these experiments, the rules are not
so strict and some of the following can be ignored on a case-by-case basis.
However, if such an experiment is to be upgraded to a default option to be
shipped by distros and used by most users, it must first meet all of the
dependency requirements.

Open Development Model
----------------------

All external dependencies except OS provided core libraries must follow an Open
Development Model where new patches are reviewed and discussed in public.
External contributors need to be able to engage with the project in public and
there has to be a streamlined path for getting patches accepted. This is
required by some distribution maintainers in order to be able to handle bugs
inside Mesa and external dependencies.

Distribution acceptance
-----------------------

All dependencies relevant for Linux builds need to have some sort of acceptance
in the biggest distributions either by being shipped already or maintainer
having stated in public that they are willing to accept new packaging of the
dependencies in question.

In addition Mesa's status as a core system component means it needs to be
careful not just for long-term maintainability but also image size. Container
images and live media are especially sensitive to increased disk footprint,
and new dependencies that significantly increase the installed system size can
expect increased resistance to acceptance in Mesa and in distributions.

LLVM
----

Dependencies which are a fork of LLVM or cannot keep up with LLVM releases are
not acceptable. We can't require distributions to package multiple LLVM forks
nor do we want dependencies to hold us back from using the latest LLVM
releases.

Specifically for backend compilers there is a strong preference of writing an
in tree NIR based compiler to keep compilation time minimal and being able to
fix bugs in Mesa without having to rely on a new LLVM release.
