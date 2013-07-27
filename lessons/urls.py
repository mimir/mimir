from django.conf.urls import patterns, url
from lessons import views

urlpatterns = patterns('',
    url(r'^$', views.index, name = 'index'),
    url(r'^check_answer/$', views.check_answer, name = 'check_answer'), #TODO sort this properly, either new url or properly ensure lessons can't be named check answer (why would they be)
    url(r'^(?P<lesson_name>\w+)/$', views.read, name = 'read'),
    url(r'^(?P<lesson_name>\w+)/test/(?P<question_id>\d+)/$', views.question, name = 'question'),
    url(r'^(?P<lesson_name>\w+)/test/$', views.rand_question, name = 'rand_question'),
    url(r'^(?P<lesson_id>\d+)/rate/$', views.rate_lesson, name = 'rate_lesson'),

)
