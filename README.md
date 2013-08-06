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

Testing
=======

Set-up
------

If you would like to run the tests on Mimir some set up is required.
I will assume you have followed the guide above already.

First of all go to:

https://pypi.python.org/pypi/selenium

and download the Selenium package to run a dumby server for testing. Next install this by going to 
command line, finding the file you downloaded it to and running

`python setup.py install`

Next if you haven't already got google Chrome, get it and come into the future.

Now got to:

https://code.google.com/p/chromedriver/downloads/list

download the correct chromedriver for your computer. Here lots of different OS vary slight in the set up
of this. If you're a windows guy, you need to put the chromedriver.exe in

<your python directory>\Scripts

if not, I am sorry bud your own your own, lots of advice about this on stack over flow.

To add urls to test whether they view or not as a registered or a unregistered user add the url extension
as a new line to the file:

lessons/tests_unregistered - for unregistered user views
lessons/tests_registered - for registered user views


Using
-----

So you now should be good to go on testing. There is a couple of test commands (from the Mimir directory),
most basic is

`python manage.py test`

it will run all the tests we have written and alot of inbuilt ones in Django, for the specific ones we 
have written use

`python manage.py test lessons.MySeleniumTests.TESTNAME`

with `TESTNAME` replace by

`test_views` - to test that all the pages accessible load
`test_user` - to test your able to register log out and in.
`test_lessons` - to test lessons work (under development)

more to follow. Feel free to add to this list if you wish to.
