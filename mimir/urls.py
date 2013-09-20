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
    url(r'^login/$', 'django.contrib.auth.views.login', name = 'login'),
    url(r'^logout/$', 'django.contrib.auth.views.logout', {'next_page': '/'}, name = 'logout'),
    url(r'^register/$', 'user_profiles.views.register', name = 'register'),
    url(r'^editprofile/$', 'user_profiles.views.edit', name = 'editprofile'),
    url(r'^users/', include('user_profiles.urls', namespace = 'user_profiles')),
    url(r'^$', 'mimir.views.splash'),
    url(r'^skills/$', 'lessons.views.skill_tree'),
    url(r'^home/$', 'mimir.views.index', name = 'home'),
    url(r'^profile/$', 'user_profiles.views.profile', name = 'profile'),
    url(r'^myskills/$', 'mimir.views.myskills', name = 'myskills'),
    url(r'^whatsnext/$', 'mimir.views.whatsnext', name = 'whatsnext'),
	url(r'^community/', include('community.urls', namespace = 'community')),
    url(r'^search/(?P<item>\w+)/$', 'mimir.views.search', name = 'search'),
)
