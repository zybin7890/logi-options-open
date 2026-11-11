#!/bin/sh
# GitHub's disposable Ubuntu runner only; never installed with the application.
set -eu
if [ "${GITHUB_ACTIONS:-}" != true ] || [ "${RUNNER_ENVIRONMENT:-}" != github-hosted ] || [ "${RUNNER_OS:-}" != Linux ]; then
    echo 'This network preparation is restricted to disposable GitHub-hosted Linux runners.' >&2
    exit 1
fi
if ! timeout 15 getent ahostsv4 archive.ubuntu.com; then
    interface=$(ip -o -4 route show to default | awk '{print $5; exit}')
    test -n "$interface"
    sudo resolvectl dns "$interface" 1.1.1.1 8.8.8.8
    sudo resolvectl domain "$interface" '~.'
    sudo resolvectl flush-caches
    timeout 15 getent ahostsv4 archive.ubuntu.com
fi
printf '%s\n' 'https://archive.ubuntu.com/ubuntu' | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
sudo apt-get -o Acquire::Retries=0 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15 update --error-on=any
