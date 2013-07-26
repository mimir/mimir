mimir
=====

Mimir is a new way to learn online.

Set-up
======

Mimir uses Django, a nifty Python based web development framework.
So to begin setting up Mimir first install Python.

Now we install Django, first grab the Django source

`wget -O django.tar.gz https://www.djangoproject.com/download/1.5.1/tarball/`

`tar xzvf django.tar.gz`

Now install

`cd Django-1.5.1/`

`sudo python setup.py install`

Now you can clone this repo and then change some settings.
The settings that need changing are in mimir/settings_local.py.default, you should edit this to have the correct settings in, and then save a copy as settings_local.py, while leaving the original unchanged.
At present we are using SQLite so the db info that needs changing is just the filename.
The settings file should also be changed to include the correct templates (TEMPLATE_DIRS) and static files directory (STATICFILES_DIRS), this path should be absolute.
Once that is correct we need to populate your database with mimirs tables, so run

`python manage.py syncdb`

Finally test it out with

`python manage.py runserver`
