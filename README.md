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
At present we are using SQLite so you should edit Mimir/settings.py to have the correct db info, most crucially the filename.
Once that is correct we need to sort out the new database bits so run

`python manage.py syncdb`

Now edit the settings file to have the correct template directories listed under TEMPLATE_DIRS.

Finally test it out with

`python manage.py runserver`
