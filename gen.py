#!/usr/bin/env python3
import sys
import os

# Добавляем папку game/ в путь
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'game'))

from ds_compiler import main

if __name__ == '__main__':
    main()
