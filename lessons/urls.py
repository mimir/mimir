from django.conf.urls import patterns, url
from lessons import views

urlpatterns = patterns('',
    url(r'^$', views.index, name  = 'index'),
    url(r'^(?P<lesson_id>\d+)/$', views.read, name  = 'read'),
    url(r'^(?P<lesson_id>\d+)/question/$', views.question, name  = 'question'),

)
