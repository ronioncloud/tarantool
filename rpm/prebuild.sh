set_python3() {
    python3_default=/usr/bin/python3
    for v in `seq 9 -1 4`
    do
      python3=/usr/bin/python3.$v
      if [[ -e $python ]] && [[ ! -e $python3_default ]]; then
        echo "Create symlink for Python 3: $python3_default --> $python3"
        sudo ln -s $python3_default $python3
        break
      fi
    done
}

if type yum; then
    VER=$(rpm -E %{rhel})
    # No testing on CentOS 6
    [[ $VER == 6 ]] && exit 0
    sudo yum install -y python3
    set_python3
    if [[ $VER == 7 ]]; then
        sudo yum install -y python3-devel gcc
        python3 -m pip install --user -r test-run/requirements.txt
    fi
elif type dnf; then
    VER=$(rpm -E %{rhel})
    # No testing on CentOS 6
    [[ $VER == 6 ]] && exit 0
    sudo dnf install -y python3
    set_python3
    if [[ $VER == 7 ]]; then
        sudo dnf install -y python3-devel gcc
        python3 -m pip install --user -r test-run/requirements.txt
    fi
else
    echo "No suitable package manager found"
    exit 1
fi
