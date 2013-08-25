from django.contrib import admin
from lessons.models import *

admin.site.register(Reference)
admin.site.register(Lesson)
admin.site.register(Question)
admin.site.register(Example)
admin.site.register(AnswerFormat)
admin.site.register(LessonFollowsFromLesson)
admin.site.register(LessonReferencesReference)
admin.site.register(Topic)
admin.site.register(Course)
admin.site.register(CourseIsOnTopic)
