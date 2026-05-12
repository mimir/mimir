(function () {
    'use strict';

    const switcher = document.querySelector('.version-switcher');
    if (!switcher) return;

    const button = switcher.querySelector('.version-switcher__button');
    const menu = switcher.querySelector('.version-switcher__menu');
    const manifestUrl = switcher.dataset.manifest;
    if (!button || !menu || !manifestUrl) return;

    const normalizeRoot = href => {
        const pathname = new URL(href, window.location.origin).pathname.replace(/\/+/g, '/');
        return pathname.endsWith('/') ? pathname : pathname + '/';
    };

    const currentPath = window.location.pathname.replace(/\/+/g, '/');

    const relativePath = root => {
        if (root === '/') return currentPath.replace(/^\/+/, '');
        if (!currentPath.startsWith(root)) return null;
        return currentPath.slice(root.length);
    };

    const setExpanded = expanded => {
        switcher.classList.toggle('version-switcher--open', expanded);
        button.setAttribute('aria-expanded', expanded ? 'true' : 'false');
        menu.hidden = !expanded;
    };

    const targetUrl = (root, page) => {
        const url = new URL(window.location.origin);
        url.pathname = root + page;
        url.search = window.location.search;
        url.hash = window.location.hash;
        return url;
    };

    const versionExists = async url => {
        if (url.pathname.endsWith('/')) return true;
        try {
            const response = await fetch(url, { method: 'HEAD', cache: 'no-store' });
            return response.ok;
        } catch {
            return false;
        }
    };

    const makeCurrentLabel = label => {
        const span = document.createElement('span');
        span.className = 'version-switcher__label version-switcher__label--current';
        span.textContent = label;
        return span;
    };

    const makeMissingLabel = label => {
        const span = document.createElement('span');
        span.className = 'version-switcher__label version-switcher__label--missing';
        span.textContent = label;
        return span;
    };

    const makeLink = (label, url) => {
        const link = document.createElement('a');
        link.className = 'version-switcher__link';
        link.href = url.toString();
        link.textContent = label;
        return link;
    };

    const loadSwitcher = async () => {
        const response = await fetch(manifestUrl, { cache: 'no-store' });
        if (!response.ok) throw new Error('missing versions manifest');

        const entries = (await response.json())
            .filter(entry => typeof entry.label === 'string' && typeof entry.href === 'string')
            .map(entry => ({ label: entry.label, root: normalizeRoot(entry.href) }));

        if (entries.length < 2) {
            switcher.classList.add('version-switcher--disabled');
            button.disabled = true;
            return;
        }

        const current = entries
            .map(entry => ({ ...entry, page: relativePath(entry.root) }))
            .filter(entry => entry.page !== null)
            .sort((lhs, rhs) => rhs.root.length - lhs.root.length)[0];

        if (!current) {
            switcher.classList.add('version-switcher--disabled');
            button.disabled = true;
            return;
        }

        button.textContent = current.label;

        const items = await Promise.all(entries.map(async entry => {
            if (entry.label === current.label) return makeCurrentLabel(entry.label);

            const url = targetUrl(entry.root, current.page);
            if (await versionExists(url)) return makeLink(entry.label, url);

            return makeMissingLabel(entry.label);
        }));

        menu.replaceChildren(...items);

        button.addEventListener('click', () => {
            if (switcher.classList.contains('version-switcher--disabled')) return;
            setExpanded(menu.hidden);
        });

        document.addEventListener('click', event => {
            if (!switcher.contains(event.target)) setExpanded(false);
        });

        document.addEventListener('keydown', event => {
            if (event.key === 'Escape') setExpanded(false);
        });
    };

    loadSwitcher().catch(() => {
        switcher.classList.add('version-switcher--disabled');
        button.disabled = true;
    });
})();
