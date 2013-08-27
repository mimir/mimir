from django.conf.urls import patterns, url
from community import views

urlpatterns = patterns('',
    url(r'^$', views.index, name = 'index'),
    url(r'^(?P<question_id>\d+)/$', views.question, name='question'),
    url(r'^vote/$', views.rate_user_item, name = 'rate_user_item'),
    url(r'^comment/$', views.add_comment, name = 'comment'),
    url(r'^ask/$', views.ask_question, name = 'ask'),

    #url(r'^(?P<user_name>\w+)/$', views.view, name = 'view'),
    #url(r'^(?P<lesson_id>\d+)/question/$', views.question, name = 'question'),

)
