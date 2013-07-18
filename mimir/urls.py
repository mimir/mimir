from django.conf.urls import patterns, include, url

# Uncomment the next two lines to enable the admin:
from django.contrib import admin
admin.autodiscover()

urlpatterns = patterns('',
    # Examples:
    # url(r'^$', 'mimir.views.home', name='home'),
    # url(r'^mimir/', include('mimir.foo.urls')),
    url(r'^lessons/', include('lessons.urls', namespace = 'lessons')),

    # Uncomment the admin/doc line below to enable admin documentation:
    # url(r'^admin/doc/', include('django.contrib.admindocs.urls')),

    # Uncomment the next line to enable the admin:
    url(r'^admin/', include(admin.site.urls)),
    url(r'^users/login/$', 'django.contrib.auth.views.login', name = 'login'),
    url(r'^users/logout/$', 'django.contrib.auth.views.logout', {'next_page': '/'}, name = 'logout'),
    url(r'^users/signup/$', 'mimir.views.register', name = 'signup'),
    url(r'^$', 'mimir.views.splash'),
    url(r'^home/$', 'mimir.views.index', name = 'home'),
)
