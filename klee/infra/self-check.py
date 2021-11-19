#!/usr/bin/env python

from expmanager import ExperimentManager
from argparse import ArgumentParser
from expmanager import DEFAULT_BASE_PORT

DEFAULT_HOSTS = "self-check"
DEFAULT_CMDLINES = "self-check"
DEFAULT_EXP = "self-check"
DEFAULT_KLEECMD = "self-check"
DEFAULT_COVERABLE = "self-check"

UID_PREFIX = "self-check"
DURATION = 600
STRATEGY = "random-path,cov-opt,partitioning"

def main():
    parser = ArgumentParser(description="Run the self-check suite.")

    parser.add_argument("--hosts", default=DEFAULT_HOSTS, help="Available cluster machines")
    parser.add_argument("--cmdlines", default=DEFAULT_CMDLINES, help="Command lines of the testing targets")
    parser.add_argument("--exp", default=DEFAULT_EXP, help="The experiment schedule file")
    parser.add_argument("--kleecmd", default=DEFAULT_KLEECMD, help="The command line parameters to pass to Klee")
    parser.add_argument("--coverable", default=DEFAULT_COVERABLE, help="The file containing coverable files")

    args = parser.parse_args()

    manager = ExperimentManager(args.hosts, args.cmdlines, args.exp,
                                args.kleecmd, args.coverable,
                                debugcomm=False, uidprefix=UID_PREFIX,
                                duration=DURATION,
                                balancetout=None,
                                strategy=STRATEGY,
                                basePort=DEFAULT_BASE_PORT)

    manager.initHosts()
    manager.runExperiment()

if __name__ == "__main__":
    main()
