"""Loading: reading source files and resolving imports."""

import os
import re

from .tables import STD_NAMESPACES
from .lexer import open_parens, split_top, strip_comment


class LoaderMixin:
    """Loading: reading source files and resolving imports."""

    def _load(self, paths):
        self.sources = [os.path.abspath(p) for p in paths]
        queue = list(paths)
        seen = set()
        while queue:
            p = queue.pop(0)
            ap = os.path.abspath(p)
            if ap in seen:
                continue
            seen.add(ap)
            pending = ''
            try:
                with open(p, 'r', encoding='utf-8-sig') as f:
                    for raw in f:
                        line = strip_comment(raw).strip()
                        if not line:
                            continue
                        if pending:
                            line = pending + ' ' + line
                        if open_parens(line) > 0:
                            pending = line
                            continue
                        pending = ''
                        for q in (s.strip() for s in split_top(line, ';')):
                            if not q:
                                continue
                            imp = re.match(r'^import\s+([\w.]+)$', q)
                            if imp:
                                extra = self._resolve_import(imp.group(1), p)
                                if extra:
                                    queue.append(extra)
                                continue
                            self.lines.append(q)
            except OSError as e:
                self._error(f"cannot read '{p}': {e}")
                return False
            if pending:
                self._error(f"unfinished call in '{p}' (unclosed parenthesis): {pending}")
        return True

    def _resolve_import(self, path, from_file):
        """import Dim.System.Gui maps to Dim/System/Gui.ds next to the sources."""
        ns = path.split('.')[-1]
        if path in STD_NAMESPACES:
            self.imports[ns] = path
            return None
        roots = [os.path.dirname(os.path.abspath(from_file))]
        roots += [os.path.dirname(s) for s in self.sources]
        roots.append(os.path.dirname(os.path.abspath(__file__)))
        rel = os.path.join(*path.split('.')) + '.ds'
        for root in roots:
            cur = root
            for _ in range(4):
                for cand in (os.path.join(cur, rel), os.path.join(cur, 'std', rel)):
                    if os.path.isfile(cand):
                        self.imports[ns] = path
                        return cand
                cur = os.path.dirname(cur)
        # Resolved later: the project itself may declare a class with that name.
        self.imports[ns] = path
        return None
