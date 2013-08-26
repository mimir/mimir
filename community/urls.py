from django.conf.urls import patterns, url
from community import views

urlpatterns = patterns('',
    url(r'^$', views.index, name = 'index'),
    url(r'^questions/(?P<question_id>\d+)/$', views.question, name='question'),
    url(r'^questions/vote/$', views.rate_user_item, name = 'rate_user_item'),

    #url(r'^(?P<user_name>\w+)/$', views.view, name = 'view'),
    #url(r'^(?P<lesson_id>\d+)/question/$', views.question, name = 'question'),

)
